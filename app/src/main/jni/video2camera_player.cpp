#include "video2camera_player.h"

#include <android/log.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "libyuv_runtime.h"
#include "video2camera_service.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

constexpr int64_t kCodecTimeoutUs = 10000;
constexpr int32_t kColorFormatYuv420Planar = 19;
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;
constexpr int32_t kColorFormatYuv420Flexible = 0x7F420888;
constexpr int32_t kColorFormatQcomYuv420SemiPlanar = 0x7FA30C00;
constexpr int32_t kColorFormatQcomYuv420SemiPlanar32m = 0x7FA30C04;

struct PlaybackState {
  std::mutex mutex;
  std::thread worker;
  bool running = false;
  bool stop_requested = false;
  uint64_t session_id = 0;
  std::string path;
};

PlaybackState g_playback_state;
std::atomic<uint64_t> g_image_to_i420_perf_count{0};
std::atomic<uint64_t> g_codec_to_i420_perf_count{0};

double duration_ms(std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

struct ScopedFd {
  int fd = -1;
  ~ScopedFd() {
    if (fd >= 0) close(fd);
  }
};

struct CodecOutputLayout {
  int32_t width = 0;
  int32_t height = 0;
  int32_t crop_left = 0;
  int32_t crop_top = 0;
  int32_t crop_right = -1;
  int32_t crop_bottom = -1;
  int32_t stride = 0;
  int32_t slice_height = 0;
  int32_t color_format = 0;
};

bool should_stop_locked(uint64_t session_id) {
  return g_playback_state.stop_requested || !g_playback_state.running ||
         g_playback_state.session_id != session_id;
}

bool ShouldStop(uint64_t session_id) {
  std::lock_guard<std::mutex> lock(g_playback_state.mutex);
  return should_stop_locked(session_id);
}

void MarkWorkerExited(uint64_t session_id) {
  std::lock_guard<std::mutex> lock(g_playback_state.mutex);
  if (g_playback_state.session_id == session_id) {
    g_playback_state.running = false;
  }
}

void copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
                int pixel_stride, int width, int height) {
  for (int y = 0; y < height; ++y) {
    const uint8_t *src_row = src + static_cast<size_t>(y) * src_stride;
    uint8_t *dst_row = dst + static_cast<size_t>(y) * dst_stride;
    if (pixel_stride == 1) {
      memcpy(dst_row, src_row, static_cast<size_t>(width));
      continue;
    }
    for (int x = 0; x < width; ++x) {
      dst_row[x] = src_row[x * pixel_stride];
    }
  }
}

bool ConvertImageToI420(AImage *image, std::vector<uint8_t> *out, int32_t *out_width,
                        int32_t *out_height) {
  if (image == nullptr || out == nullptr || out_width == nullptr || out_height == nullptr) {
    return false;
  }
  const auto perf_start = std::chrono::steady_clock::now();

  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  if (AImage_getWidth(image, &width) != AMEDIA_OK ||
      AImage_getHeight(image, &height) != AMEDIA_OK ||
      AImage_getFormat(image, &format) != AMEDIA_OK) {
    return false;
  }
  if (width <= 0 || height <= 0 || format != AIMAGE_FORMAT_YUV_420_888) {
    return false;
  }

  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_size = static_cast<size_t>(chroma_width) * chroma_height;
  out->resize(y_size + 2 * chroma_size);

  uint8_t *dst_y = out->data();
  uint8_t *dst_u = dst_y + y_size;
  uint8_t *dst_v = dst_u + chroma_size;

  uint8_t *src_data[3] = {};
  int data_len[3] = {};
  int row_stride[3] = {};
  int pixel_stride[3] = {1, 1, 1};
  for (int plane = 0; plane < 3; ++plane) {
    if (AImage_getPlaneData(image, plane, &src_data[plane], &data_len[plane]) != AMEDIA_OK ||
        src_data[plane] == nullptr || data_len[plane] <= 0 ||
        AImage_getPlaneRowStride(image, plane, &row_stride[plane]) != AMEDIA_OK) {
      return false;
    }
    if (plane != 0 &&
        AImage_getPlanePixelStride(image, plane, &pixel_stride[plane]) != AMEDIA_OK) {
      return false;
    }
  }

  bool libyuv_used = false;
  if (pixel_stride[1] == pixel_stride[2] &&
      LibYuvAndroid420ToI420(src_data[0], row_stride[0], src_data[1],
                             row_stride[1], src_data[2], row_stride[2],
                             pixel_stride[1], dst_y, width, dst_u, chroma_width,
                             dst_v, chroma_width, width, height)) {
    libyuv_used = true;
    *out_width = width;
    *out_height = height;
    const uint64_t count =
        g_image_to_i420_perf_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 5 || (count % 120) == 0) {
      LOGI("Perf AImageToI420 #%llu %dx%d libyuv=%d ms=%.3f",
           static_cast<unsigned long long>(count), width, height,
           libyuv_used ? 1 : 0,
           duration_ms(perf_start, std::chrono::steady_clock::now()));
    }
    return true;
  }

  copy_plane(dst_y, width, src_data[0], row_stride[0], 1, width, height);
  copy_plane(dst_u, chroma_width, src_data[1], row_stride[1], pixel_stride[1],
             chroma_width, chroma_height);
  copy_plane(dst_v, chroma_width, src_data[2], row_stride[2], pixel_stride[2],
             chroma_width, chroma_height);

  *out_width = width;
  *out_height = height;
  const uint64_t count =
      g_image_to_i420_perf_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 5 || (count % 120) == 0) {
    LOGI("Perf AImageToI420 #%llu %dx%d libyuv=%d ms=%.3f",
         static_cast<unsigned long long>(count), width, height, 0,
         duration_ms(perf_start, std::chrono::steady_clock::now()));
  }
  return true;
}

size_t i420_size_for(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) return 0;
  return static_cast<size_t>(width) * height +
         2 * static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2);
}

void CopyPlaneCrop(uint8_t *dst, int dst_stride, const uint8_t *src,
                   int src_stride, int width, int height) {
  for (int y = 0; y < height; ++y) {
    memcpy(dst + static_cast<size_t>(y) * dst_stride,
           src + static_cast<size_t>(y) * src_stride,
           static_cast<size_t>(width));
  }
}

void SplitNv12Crop(uint8_t *dst_u, uint8_t *dst_v, int dst_stride,
                   const uint8_t *src_uv, int src_stride, int width,
                   int height) {
  for (int y = 0; y < height; ++y) {
    const uint8_t *src_row = src_uv + static_cast<size_t>(y) * src_stride;
    uint8_t *u_row = dst_u + static_cast<size_t>(y) * dst_stride;
    uint8_t *v_row = dst_v + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < width; ++x) {
      u_row[x] = src_row[x * 2 + 0];
      v_row[x] = src_row[x * 2 + 1];
    }
  }
}

void SplitNv21Crop(uint8_t *dst_u, uint8_t *dst_v, int dst_stride,
                   const uint8_t *src_vu, int src_stride, int width,
                   int height) {
  for (int y = 0; y < height; ++y) {
    const uint8_t *src_row = src_vu + static_cast<size_t>(y) * src_stride;
    uint8_t *u_row = dst_u + static_cast<size_t>(y) * dst_stride;
    uint8_t *v_row = dst_v + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < width; ++x) {
      v_row[x] = src_row[x * 2 + 0];
      u_row[x] = src_row[x * 2 + 1];
    }
  }
}

int32_t GetFormatInt32(AMediaFormat *format, const char *key, int32_t fallback) {
  int32_t value = fallback;
  if (format != nullptr) AMediaFormat_getInt32(format, key, &value);
  return value;
}

void UpdateCodecOutputLayout(AMediaFormat *format, CodecOutputLayout *layout) {
  if (format == nullptr || layout == nullptr) return;

  layout->width = GetFormatInt32(format, AMEDIAFORMAT_KEY_WIDTH, layout->width);
  layout->height = GetFormatInt32(format, AMEDIAFORMAT_KEY_HEIGHT, layout->height);
  layout->stride = GetFormatInt32(format, AMEDIAFORMAT_KEY_STRIDE,
                                  layout->stride > 0 ? layout->stride : layout->width);
  layout->slice_height = GetFormatInt32(
      format, AMEDIAFORMAT_KEY_SLICE_HEIGHT,
      layout->slice_height > 0 ? layout->slice_height : layout->height);
  layout->color_format = GetFormatInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                                        layout->color_format);

  layout->crop_left = GetFormatInt32(format, "crop-left", 0);
  layout->crop_top = GetFormatInt32(format, "crop-top", 0);
  layout->crop_right = GetFormatInt32(
      format, "crop-right", layout->width > 0 ? layout->width - 1 : -1);
  layout->crop_bottom = GetFormatInt32(
      format, "crop-bottom", layout->height > 0 ? layout->height - 1 : -1);

  if (layout->stride <= 0) layout->stride = layout->width;
  if (layout->slice_height <= 0) layout->slice_height = layout->height;
  layout->crop_left = std::max<int32_t>(0, layout->crop_left);
  layout->crop_top = std::max<int32_t>(0, layout->crop_top);
  if (layout->crop_right < layout->crop_left && layout->width > 0) {
    layout->crop_right = layout->width - 1;
  }
  if (layout->crop_bottom < layout->crop_top && layout->height > 0) {
    layout->crop_bottom = layout->height - 1;
  }

  const char *desc = AMediaFormat_toString(format);
  LOGI("NativePlayback: CPU output layout fmt=%#x %dx%d crop=(%d,%d)-(%d,%d) stride=%d slice=%d raw=%s",
       layout->color_format, layout->width, layout->height, layout->crop_left,
       layout->crop_top, layout->crop_right, layout->crop_bottom, layout->stride,
       layout->slice_height, desc != nullptr ? desc : "(null)");
}

bool ConvertCodecOutputToI420(const uint8_t *src, size_t src_size,
                              const CodecOutputLayout &layout,
                              std::vector<uint8_t> *out, int32_t *out_width,
                              int32_t *out_height) {
  if (src == nullptr || src_size == 0 || out == nullptr || out_width == nullptr ||
      out_height == nullptr || layout.width <= 0 || layout.height <= 0 ||
      layout.stride <= 0 || layout.slice_height <= 0) {
    return false;
  }

  const auto perf_start = std::chrono::steady_clock::now();
  int32_t width = layout.crop_right - layout.crop_left + 1;
  int32_t height = layout.crop_bottom - layout.crop_top + 1;
  if (width <= 0 || height <= 0) {
    width = layout.width;
    height = layout.height;
  }
  // Keep I420 chroma math simple and decoder-compatible.
  width &= ~1;
  height &= ~1;
  if (width <= 0 || height <= 0) return false;

  const int32_t crop_left = layout.crop_left & ~1;
  const int32_t crop_top = layout.crop_top & ~1;
  const int32_t chroma_width = (width + 1) / 2;
  const int32_t chroma_height = (height + 1) / 2;
  const size_t dst_size = i420_size_for(width, height);
  out->assign(dst_size, 0);

  uint8_t *dst_y = out->data();
  uint8_t *dst_u = dst_y + static_cast<size_t>(width) * height;
  uint8_t *dst_v = dst_u + static_cast<size_t>(chroma_width) * chroma_height;

  const size_t y_plane_size =
      static_cast<size_t>(layout.stride) * layout.slice_height;
  if (src_size < y_plane_size) return false;

  const uint8_t *src_y =
      src + static_cast<size_t>(crop_top) * layout.stride + crop_left;
  CopyPlaneCrop(dst_y, width, src_y, layout.stride, width, height);

  const bool planar =
      layout.color_format == kColorFormatYuv420Planar;
  const bool semiplanar =
      layout.color_format == kColorFormatYuv420SemiPlanar ||
      layout.color_format == kColorFormatYuv420Flexible ||
      layout.color_format == kColorFormatQcomYuv420SemiPlanar ||
      layout.color_format == kColorFormatQcomYuv420SemiPlanar32m ||
      layout.color_format == 0;

  if (planar) {
    const int32_t chroma_stride = std::max<int32_t>(1, layout.stride / 2);
    const int32_t chroma_slice = std::max<int32_t>(1, layout.slice_height / 2);
    const size_t u_offset = y_plane_size;
    const size_t v_offset = u_offset + static_cast<size_t>(chroma_stride) * chroma_slice;
    const size_t needed = v_offset + static_cast<size_t>(chroma_stride) * chroma_height;
    if (src_size < needed) return false;
    const uint8_t *src_u = src + u_offset +
                           static_cast<size_t>(crop_top / 2) * chroma_stride +
                           crop_left / 2;
    const uint8_t *src_v = src + v_offset +
                           static_cast<size_t>(crop_top / 2) * chroma_stride +
                           crop_left / 2;
    CopyPlaneCrop(dst_u, chroma_width, src_u, chroma_stride, chroma_width,
                  chroma_height);
    CopyPlaneCrop(dst_v, chroma_width, src_v, chroma_stride, chroma_width,
                  chroma_height);
  } else if (semiplanar) {
    const size_t uv_offset = y_plane_size;
    const size_t needed =
        uv_offset + static_cast<size_t>(layout.stride) * chroma_height;
    if (src_size < needed) return false;
    const uint8_t *src_uv = src + uv_offset +
                            static_cast<size_t>(crop_top / 2) * layout.stride +
                            crop_left;
    // Google HW decoder CPU output is NV12 for COLOR_FormatYUV420Flexible /
    // semi-planar on Pixel. If color looks purple/green later, flip this one
    // call to SplitNv21Crop.
    SplitNv12Crop(dst_u, dst_v, chroma_width, src_uv, layout.stride,
                  chroma_width, chroma_height);
  } else {
    static std::atomic<uint64_t> unknown_count{0};
    const uint64_t count = unknown_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 5) {
      LOGW("NativePlayback: unknown CPU color format %#x; trying NV12 layout",
           layout.color_format);
    }
    const size_t uv_offset = y_plane_size;
    const size_t needed =
        uv_offset + static_cast<size_t>(layout.stride) * chroma_height;
    if (src_size < needed) return false;
    const uint8_t *src_uv = src + uv_offset +
                            static_cast<size_t>(crop_top / 2) * layout.stride +
                            crop_left;
    SplitNv12Crop(dst_u, dst_v, chroma_width, src_uv, layout.stride,
                  chroma_width, chroma_height);
  }

  *out_width = width;
  *out_height = height;
  const uint64_t count =
      g_codec_to_i420_perf_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 5 || (count % 120) == 0) {
    LOGI("Perf CodecOutputToI420 #%llu %dx%d fmt=%#x stride=%d slice=%d ms=%.3f",
         static_cast<unsigned long long>(count), width, height,
         layout.color_format, layout.stride, layout.slice_height,
         duration_ms(perf_start, std::chrono::steady_clock::now()));
  }
  return true;
}

void PacePresentation(int64_t pts_us, bool *have_clock,
                      std::chrono::steady_clock::time_point *base_wall,
                      int64_t *base_pts_us, uint64_t session_id) {
  if (pts_us < 0 || have_clock == nullptr || base_wall == nullptr ||
      base_pts_us == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!*have_clock) {
    *have_clock = true;
    *base_wall = now;
    *base_pts_us = pts_us;
    return;
  }

  const int64_t delta_us = std::max<int64_t>(0, pts_us - *base_pts_us);
  const auto target = *base_wall + std::chrono::microseconds(delta_us);
  if (target > now) {
    const auto sleep_for = target - now;
    if (sleep_for > std::chrono::milliseconds(0) && !ShouldStop(session_id)) {
      std::this_thread::sleep_for(sleep_for);
    }
  }
}

bool SelectVideoTrack(AMediaExtractor *extractor, size_t *out_track_index,
                      AMediaFormat **out_format, std::string *out_mime) {
  if (extractor == nullptr || out_track_index == nullptr || out_format == nullptr ||
      out_mime == nullptr) {
    return false;
  }

  const size_t track_count = AMediaExtractor_getTrackCount(extractor);
  for (size_t i = 0; i < track_count; ++i) {
    AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
    if (format == nullptr) continue;

    const char *mime = nullptr;
    if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) &&
        mime != nullptr && strncmp(mime, "video/", 6) == 0) {
      *out_track_index = i;
      *out_format = format;
      *out_mime = mime;
      return true;
    }

    AMediaFormat_delete(format);
  }

  return false;
}

bool DecodeOneLoop(const std::string &path, uint64_t session_id, std::string *error) {
  ScopedFd fd{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (fd.fd < 0) {
    if (error != nullptr) *error = "open failed";
    LOGE("NativePlayback: open failed for %s", path.c_str());
    return false;
  }

  AMediaExtractor *extractor = AMediaExtractor_new();
  if (extractor == nullptr) {
    if (error != nullptr) *error = "AMediaExtractor_new failed";
    return false;
  }

  AMediaFormat *track_format = nullptr;
  AMediaCodec *codec = nullptr;

  auto cleanup = [&]() {
    if (codec != nullptr) {
      AMediaCodec_stop(codec);
      AMediaCodec_delete(codec);
      codec = nullptr;
    }
    if (track_format != nullptr) {
      AMediaFormat_delete(track_format);
      track_format = nullptr;
    }
    AMediaExtractor_delete(extractor);
  };

  struct stat st{};
  if (fstat(fd.fd, &st) != 0 || st.st_size <= 0) {
    if (error != nullptr) *error = "fstat failed";
    LOGE("NativePlayback: fstat failed for %s", path.c_str());
    cleanup();
    return false;
  }

  if (AMediaExtractor_setDataSourceFd(extractor, fd.fd, 0, static_cast<off64_t>(st.st_size)) != AMEDIA_OK) {
    if (error != nullptr) *error = "setDataSourceFd failed";
    LOGE("NativePlayback: setDataSourceFd failed path=%s size=%lld", path.c_str(),
         static_cast<long long>(st.st_size));
    cleanup();
    return false;
  }

  size_t track_index = 0;
  std::string mime;
  if (!SelectVideoTrack(extractor, &track_index, &track_format, &mime)) {
    if (error != nullptr) *error = "no video track";
    cleanup();
    return false;
  }
  if (AMediaExtractor_selectTrack(extractor, track_index) != AMEDIA_OK) {
    if (error != nullptr) *error = "selectTrack failed";
    cleanup();
    return false;
  }

  int32_t width = 0;
  int32_t height = 0;
  AMediaFormat_getInt32(track_format, AMEDIAFORMAT_KEY_WIDTH, &width);
  AMediaFormat_getInt32(track_format, AMEDIAFORMAT_KEY_HEIGHT, &height);
  if (width <= 0 || height <= 0) {
    if (error != nullptr) *error = "invalid video dimensions";
    cleanup();
    return false;
  }

  // CPU ByteBuffer decode is more reliable inside cameraserver than
  // codec->AImageReader surface decode. On this Pixel 6a Android 16 build the
  // surface path reports output format but never yields AImage buffers, so the
  // hook has no ready frames. Request flexible YUV and consume output buffers
  // directly.
  AMediaFormat_setInt32(track_format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        kColorFormatYuv420Flexible);

  codec = AMediaCodec_createDecoderByType(mime.c_str());
  if (codec == nullptr) {
    if (error != nullptr) *error = "createDecoderByType failed";
    cleanup();
    return false;
  }
  if (AMediaCodec_configure(codec, track_format, nullptr, nullptr, 0) != AMEDIA_OK ||
      AMediaCodec_start(codec) != AMEDIA_OK) {
    if (error != nullptr) *error = "codec configure/start failed";
    cleanup();
    return false;
  }

  bool input_done = false;
  bool output_done = false;
  bool have_clock = false;
  int64_t base_pts_us = 0;
  auto base_wall = std::chrono::steady_clock::now();
  uint64_t produced_frames = 0;
  CodecOutputLayout output_layout;
  output_layout.width = width;
  output_layout.height = height;
  output_layout.crop_right = width - 1;
  output_layout.crop_bottom = height - 1;
  output_layout.stride = width;
  output_layout.slice_height = height;
  output_layout.color_format = kColorFormatYuv420Flexible;

  while (!ShouldStop(session_id) && !output_done) {
    if (!input_done) {
      const ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, kCodecTimeoutUs);
      if (input_index >= 0) {
        size_t input_size = 0;
        uint8_t *input_buffer = AMediaCodec_getInputBuffer(codec,
                                                           static_cast<size_t>(input_index),
                                                           &input_size);
        if (input_buffer != nullptr && input_size > 0) {
          const ssize_t sample_size =
              AMediaExtractor_readSampleData(extractor, input_buffer, input_size);
          if (sample_size < 0) {
            AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(input_index), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            input_done = true;
          } else {
            const int64_t sample_time = std::max<int64_t>(0, AMediaExtractor_getSampleTime(extractor));
            AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(input_index), 0,
                                         static_cast<size_t>(sample_size), sample_time, 0);
            AMediaExtractor_advance(extractor);
          }
        }
      }
    }

    AMediaCodecBufferInfo info{};
    const ssize_t output_index = AMediaCodec_dequeueOutputBuffer(codec, &info, kCodecTimeoutUs);
    if (output_index >= 0) {
      const bool codec_config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
      const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      const bool should_render = !codec_config && !eos;
      if (should_render) {
        PacePresentation(info.presentationTimeUs, &have_clock, &base_wall, &base_pts_us,
                         session_id);
        size_t output_size = 0;
        uint8_t *output_buffer =
            AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(output_index),
                                        &output_size);
        if (output_buffer != nullptr && output_size > 0 &&
            info.offset >= 0 && info.size > 0 &&
            static_cast<size_t>(info.offset) < output_size) {
          const size_t available_size =
              std::min(output_size - static_cast<size_t>(info.offset),
                       static_cast<size_t>(info.size));
          std::vector<uint8_t> i420;
          int32_t frame_width = 0;
          int32_t frame_height = 0;
          if (ConvertCodecOutputToI420(output_buffer + info.offset, available_size,
                                       output_layout, &i420, &frame_width,
                                       &frame_height)) {
            PublishDecodedI420Frame(frame_width, frame_height, std::move(i420));
            produced_frames += 1;
            if (produced_frames <= 5 || (produced_frames % 120) == 0) {
              LOGI("NativePlayback: decoded CPU frame #%llu %dx%d pts=%lld from %s",
                   static_cast<unsigned long long>(produced_frames), frame_width,
                   frame_height, static_cast<long long>(info.presentationTimeUs),
                   path.c_str());
            }
          } else if (produced_frames < 5) {
            LOGW("NativePlayback: CPU output convert failed size=%zu info.offset=%d info.size=%d fmt=%#x",
                 output_size, info.offset, info.size, output_layout.color_format);
          }
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
      } else {
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
      }
      output_done = eos;
    } else if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *output_format = AMediaCodec_getOutputFormat(codec);
      if (output_format != nullptr) {
        const char *desc = AMediaFormat_toString(output_format);
        LOGI("NativePlayback: output format changed %s",
             desc != nullptr ? desc : "(null)");
        UpdateCodecOutputLayout(output_format, &output_layout);
        AMediaFormat_delete(output_format);
      }
    }
  }

  cleanup();
  return !ShouldStop(session_id);
}

void PlaybackWorkerMain(uint64_t session_id, std::string path) {
  LOGI("NativePlayback: worker start session=%llu path=%s",
       static_cast<unsigned long long>(session_id), path.c_str());
  ClearLatestBinderFrame();
  while (!ShouldStop(session_id)) {
    std::string error;
    const bool should_continue = DecodeOneLoop(path, session_id, &error);
    if (!should_continue) {
      if (!error.empty()) {
        LOGE("NativePlayback: session=%llu failed: %s",
             static_cast<unsigned long long>(session_id), error.c_str());
      }
      break;
    }
    if (!ShouldStop(session_id)) {
      LOGI("NativePlayback: loop reached EOF, restarting %s", path.c_str());
    }
  }
  ClearLatestBinderFrame();
  MarkWorkerExited(session_id);
  LOGI("NativePlayback: worker stop session=%llu",
       static_cast<unsigned long long>(session_id));
}

}  // namespace

bool StartNativePlayback(const std::string &path, std::string *error) {
  if (path.empty()) {
    if (error != nullptr) *error = "empty path";
    return false;
  }

  std::thread previous_worker;
  uint64_t next_session = 0;
  {
    std::lock_guard<std::mutex> lock(g_playback_state.mutex);
    if (g_playback_state.worker.joinable()) {
      g_playback_state.stop_requested = true;
      previous_worker = std::move(g_playback_state.worker);
    }
  }
  if (previous_worker.joinable()) previous_worker.join();

  {
    std::lock_guard<std::mutex> lock(g_playback_state.mutex);
    g_playback_state.stop_requested = false;
    g_playback_state.running = true;
    g_playback_state.path = path;
    g_playback_state.session_id += 1;
    next_session = g_playback_state.session_id;
    g_playback_state.worker = std::thread(PlaybackWorkerMain, next_session, path);
  }

  return true;
}

void StopNativePlayback() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(g_playback_state.mutex);
    g_playback_state.stop_requested = true;
    g_playback_state.running = false;
    if (g_playback_state.worker.joinable()) {
      worker = std::move(g_playback_state.worker);
    }
  }
  if (worker.joinable()) worker.join();
  ClearLatestBinderFrame();
}

bool IsNativePlaybackRunning() {
  std::lock_guard<std::mutex> lock(g_playback_state.mutex);
  return g_playback_state.running && !g_playback_state.stop_requested;
}

}  // namespace awesomecam
