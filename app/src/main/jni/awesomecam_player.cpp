#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_stop{false};

struct Options {
  std::string input = "/data/camera/input.mp4";
  std::string pidfile = "/data/camera/awesomecam_player.pid";
  bool loop = true;
  int max_long = 1920;
  int max_short = 1080;
};

struct BinderClient {
  awesomecam::BinderRuntimeApi api{};
  AIBinder_Class *client_class = nullptr;
  AIBinder *remote = nullptr;
};

struct SourceRing {
  int fd = -1;
  void *addr = nullptr;
  size_t size = 0;
  int32_t width = 0;
  int32_t height = 0;
  uint32_t slot_size = 0;
  uint32_t next_slot = 0;
  uint64_t next_generation = 0;

  ~SourceRing() { reset(); }

  void reset() {
    if (addr != nullptr && addr != MAP_FAILED && size > 0) munmap(addr, size);
    addr = nullptr;
    size = 0;
    if (fd >= 0) close(fd);
    fd = -1;
  }

  awesomecam::SharedMemoryRingHeader *header() const {
    return reinterpret_cast<awesomecam::SharedMemoryRingHeader *>(addr);
  }
};

void SignalHandler(int) { g_stop.store(true, std::memory_order_release); }

void *ClientOnCreate(void *) { return nullptr; }
void ClientOnDestroy(void *) {}
binder_status_t ClientOnTransact(AIBinder *, transaction_code_t, const AParcel *, AParcel *) {
  return STATUS_UNKNOWN_TRANSACTION;
}

void AtomicStoreRelease(uint64_t *ptr, uint64_t value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}
void AtomicStoreRelease(uint32_t *ptr, uint32_t value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}
void AtomicStoreRelease(int64_t *ptr, int64_t value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

int64_t MonotonicUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

void PrintUsage() {
  LOGI("FFmpegPlayer usage: awesomecam_player --input /data/camera/input.mp4 --loop --pidfile /data/camera/awesomecam_player.pid");
}

Options ParseOptions(int argc, char **argv) {
  Options opt{};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i] ? argv[i] : "";
    auto need_value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        LOGW("FFmpegPlayer missing value for %s", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--input") {
      if (const char *v = need_value("--input")) opt.input = v;
    } else if (arg == "--pidfile") {
      if (const char *v = need_value("--pidfile")) opt.pidfile = v;
    } else if (arg == "--once") {
      opt.loop = false;
    } else if (arg == "--loop") {
      opt.loop = true;
    } else if (arg == "--max-long") {
      if (const char *v = need_value("--max-long")) opt.max_long = std::max(2, atoi(v));
    } else if (arg == "--max-short") {
      if (const char *v = need_value("--max-short")) opt.max_short = std::max(2, atoi(v));
    } else if (arg == "--service") {
      (void)need_value("--service");
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
    }
  }
  return opt;
}

void WritePidFile(const std::string &path) {
  if (path.empty()) return;
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    LOGW("FFmpegPlayer: open pidfile %s failed errno=%d", path.c_str(), errno);
    return;
  }
  char buf[64];
  const int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
  if (len > 0) (void)write(fd, buf, static_cast<size_t>(len));
  close(fd);
}

bool ConnectService(BinderClient *client) {
  if (client == nullptr) return false;
  if (!awesomecam::LoadBinderRuntimeApi(&client->api)) {
    LOGE("FFmpegPlayer: failed to load binder runtime");
    return false;
  }
  if (client->client_class == nullptr) {
    client->client_class = client->api.binder_class_define(
        awesomecam::kVideo2CameraDescriptor, ClientOnCreate, ClientOnDestroy, ClientOnTransact);
    if (client->client_class == nullptr) {
      LOGE("FFmpegPlayer: failed to define binder client class");
      return false;
    }
  }
  client->remote = client->api.wait_for_service(awesomecam::kVideo2CameraServiceName);
  if (client->remote == nullptr) {
    LOGE("FFmpegPlayer: Video2CameraService unavailable");
    return false;
  }
  if (!client->api.binder_associate_class(client->remote, client->client_class)) {
    LOGE("FFmpegPlayer: AIBinder_associateClass failed remote=%p", client->remote);
    return false;
  }
  LOGI("FFmpegPlayer: connected to %s remote=%p", awesomecam::kVideo2CameraServiceName,
       client->remote);
  return true;
}

bool RegisterSourceRing(BinderClient *client, const SourceRing &ring) {
  if (client == nullptr || client->remote == nullptr || ring.fd < 0) return false;
  AParcel *in = nullptr;
  AParcel *out = nullptr;
  binder_status_t status = client->api.binder_prepare_transaction(client->remote, &in);
  const char *failed_step = "prepare";
  if (status == STATUS_OK) { failed_step = "write_fd"; status = client->api.parcel_write_fd(in, ring.fd); }
  if (status == STATUS_OK) { failed_step = "write_width"; status = client->api.parcel_write_int32(in, ring.width); }
  if (status == STATUS_OK) { failed_step = "write_height"; status = client->api.parcel_write_int32(in, ring.height); }
  if (status == STATUS_OK) { failed_step = "write_format"; status = client->api.parcel_write_int32(in, awesomecam::kFrameFormatI420); }
  if (status == STATUS_OK) {
    failed_step = "write_slots";
    status = client->api.parcel_write_int32(in, static_cast<int32_t>(awesomecam::kSharedMemoryRingSlotCount));
  }
  if (status == STATUS_OK) { failed_step = "write_slot_size"; status = client->api.parcel_write_int32(in, static_cast<int32_t>(ring.slot_size)); }
  if (status == STATUS_OK) { failed_step = "write_region_size"; status = client->api.parcel_write_int64(in, static_cast<int64_t>(ring.size)); }
  if (status == STATUS_OK) {
    failed_step = "transact";
    status = client->api.binder_transact(client->remote, awesomecam::kTxnRegisterSourceMemory,
                                         &in, &out, 0);
  }
  int32_t ack = 0;
  if (status == STATUS_OK && out != nullptr) client->api.parcel_read_int32(out, &ack);
  if (in != nullptr) client->api.parcel_delete(in);
  if (out != nullptr) client->api.parcel_delete(out);
  if (status != STATUS_OK || ack != 1) {
    LOGE("FFmpegPlayer: register source ring failed step=%s status=%d ack=%d", failed_step, status, ack);
    return false;
  }
  LOGI("FFmpegPlayer: source ring registered %dx%d slot=%u region=%zu",
       ring.width, ring.height, ring.slot_size, ring.size);
  return true;
}

void ClearService(BinderClient *client) {
  if (client == nullptr || client->remote == nullptr) return;
  AParcel *in = nullptr;
  AParcel *out = nullptr;
  if (client->api.binder_prepare_transaction(client->remote, &in) == STATUS_OK) {
    (void)client->api.binder_transact(client->remote, awesomecam::kTxnClear, &in, &out, 0);
  }
  if (in != nullptr) client->api.parcel_delete(in);
  if (out != nullptr) client->api.parcel_delete(out);
}

bool InitSourceRing(SourceRing *ring, int32_t width, int32_t height) {
  if (ring == nullptr || width <= 0 || height <= 0) return false;
  ring->reset();
  const size_t slot_size = awesomecam::I420FrameSize(width, height);
  const size_t region_size = awesomecam::SharedMemoryRingSize(width, height);
  if (slot_size == 0 || region_size <= sizeof(awesomecam::SharedMemoryRingHeader)) return false;

  ring->fd = ASharedMemory_create("awesomecam_ffmpeg_source", region_size);
  if (ring->fd < 0) {
    LOGE("FFmpegPlayer: ASharedMemory_create failed errno=%d", errno);
    return false;
  }
  ring->addr = mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
  if (ring->addr == MAP_FAILED) {
    LOGE("FFmpegPlayer: mmap source ring failed errno=%d", errno);
    ring->addr = nullptr;
    ring->reset();
    return false;
  }
  ring->size = region_size;
  ring->width = width;
  ring->height = height;
  ring->slot_size = static_cast<uint32_t>(slot_size);
  auto *header = ring->header();
  memset(header, 0, sizeof(*header));
  header->magic = awesomecam::kSharedMemoryRingMagic;
  header->version = awesomecam::kSharedMemoryRingVersion;
  header->header_size = sizeof(awesomecam::SharedMemoryRingHeader);
  header->slot_count = awesomecam::kSharedMemoryRingSlotCount;
  header->width = width;
  header->height = height;
  header->format = awesomecam::kFrameFormatI420;
  header->slot_size = ring->slot_size;
  header->latest_slot = awesomecam::kSharedMemoryNoSlot;
  for (uint32_t i = 0; i < awesomecam::kSharedMemoryRingSlotCount; ++i) {
    header->slots[i].data_offset =
        static_cast<uint32_t>(sizeof(awesomecam::SharedMemoryRingHeader) + i * slot_size);
    header->slots[i].data_size = ring->slot_size;
  }
  return true;
}

bool WriteFrame(SourceRing *ring, const uint8_t *i420, size_t size, int64_t pts_us) {
  if (ring == nullptr || ring->addr == nullptr || i420 == nullptr || size < ring->slot_size) {
    return false;
  }
  auto *header = ring->header();
  const uint32_t slot_index = ring->next_slot++ % awesomecam::kSharedMemoryRingSlotCount;
  const uint64_t generation = ++ring->next_generation;
  awesomecam::SharedMemoryRingSlot &slot = header->slots[slot_index];
  AtomicStoreRelease(&slot.end_generation, static_cast<uint64_t>(0));
  AtomicStoreRelease(&slot.begin_generation, generation);
  AtomicStoreRelease(&slot.pts_us, pts_us);
  AtomicStoreRelease(&slot.data_size, ring->slot_size);
  uint8_t *dst = static_cast<uint8_t *>(ring->addr) + slot.data_offset;
  memcpy(dst, i420, ring->slot_size);
  AtomicStoreRelease(&slot.data_size, ring->slot_size);
  AtomicStoreRelease(&slot.pts_us, pts_us);
  AtomicStoreRelease(&slot.end_generation, generation);
  AtomicStoreRelease(&header->latest_slot, slot_index);
  AtomicStoreRelease(&header->latest_generation, generation);
  return true;
}

void ComputeOutputSize(int src_w, int src_h, int max_long, int max_short,
                       int *out_w, int *out_h) {
  int long_edge = std::max(src_w, src_h);
  int short_edge = std::min(src_w, src_h);
  double scale = 1.0;
  if (long_edge > max_long) scale = std::min(scale, static_cast<double>(max_long) / long_edge);
  if (short_edge > max_short) scale = std::min(scale, static_cast<double>(max_short) / short_edge);
  int w = std::max(2, static_cast<int>(src_w * scale + 0.5));
  int h = std::max(2, static_cast<int>(src_h * scale + 0.5));
  w &= ~1;
  h &= ~1;
  *out_w = std::max(2, w);
  *out_h = std::max(2, h);
}

int64_t FramePtsUs(const AVFrame *frame, AVRational time_base) {
  int64_t pts = frame->best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE) pts = frame->pts;
  if (pts == AV_NOPTS_VALUE) return -1;
  return av_rescale_q(pts, time_base, AVRational{1, 1000000});
}

void PaceFrame(int64_t pts_us, bool *clock_started, int64_t *base_wall_us,
               int64_t *base_pts_us) {
  if (pts_us < 0 || clock_started == nullptr || base_wall_us == nullptr ||
      base_pts_us == nullptr) {
    return;
  }
  const int64_t now_us = MonotonicUs();
  if (!*clock_started) {
    *clock_started = true;
    *base_wall_us = now_us;
    *base_pts_us = pts_us;
    return;
  }
  const int64_t target_us = *base_wall_us + std::max<int64_t>(0, pts_us - *base_pts_us);
  const int64_t sleep_us = target_us - now_us;
  if (sleep_us > 1000 && !g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
}

bool DecodeOnce(const Options &opt, BinderClient *binder) {
  AVFormatContext *fmt = nullptr;
  AVCodecContext *codec_ctx = nullptr;
  AVPacket *pkt = nullptr;
  AVFrame *frame = nullptr;
  AVFrame *dst_frame = nullptr;
  SwsContext *sws = nullptr;
  SourceRing ring;
  std::vector<uint8_t> dst_i420;
  bool ok = false;

  auto cleanup = [&]() {
    if (binder != nullptr) ClearService(binder);
    if (sws != nullptr) sws_freeContext(sws);
    if (dst_frame != nullptr) av_frame_free(&dst_frame);
    if (frame != nullptr) av_frame_free(&frame);
    if (pkt != nullptr) av_packet_free(&pkt);
    if (codec_ctx != nullptr) avcodec_free_context(&codec_ctx);
    if (fmt != nullptr) avformat_close_input(&fmt);
  };

  LOGI("FFmpegPlayer: open input %s", opt.input.c_str());
  if (avformat_open_input(&fmt, opt.input.c_str(), nullptr, nullptr) < 0) {
    LOGE("FFmpegPlayer: avformat_open_input failed path=%s", opt.input.c_str());
    cleanup();
    return false;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    LOGE("FFmpegPlayer: avformat_find_stream_info failed");
    cleanup();
    return false;
  }
  const AVCodec *decoder = nullptr;
  const int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                               &decoder, 0);
  if (stream_index < 0 || decoder == nullptr) {
    LOGE("FFmpegPlayer: no video stream found");
    cleanup();
    return false;
  }
  AVStream *stream = fmt->streams[stream_index];
  codec_ctx = avcodec_alloc_context3(decoder);
  if (codec_ctx == nullptr || avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0 ||
      avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
    LOGE("FFmpegPlayer: decoder open failed");
    cleanup();
    return false;
  }

  int dst_w = 0;
  int dst_h = 0;
  ComputeOutputSize(codec_ctx->width, codec_ctx->height, opt.max_long, opt.max_short,
                    &dst_w, &dst_h);
  if (!InitSourceRing(&ring, dst_w, dst_h) || !RegisterSourceRing(binder, ring)) {
    cleanup();
    return false;
  }
  dst_i420.resize(awesomecam::I420FrameSize(dst_w, dst_h));
  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  dst_frame = av_frame_alloc();
  if (pkt == nullptr || frame == nullptr || dst_frame == nullptr || dst_i420.empty()) {
    LOGE("FFmpegPlayer: allocation failed");
    cleanup();
    return false;
  }

  LOGI("FFmpegPlayer: stream=%d codec=%s src=%dx%d pixFmt=%d out=%dx%d timeBase=%d/%d",
       stream_index, decoder->name ? decoder->name : "?", codec_ctx->width, codec_ctx->height,
       codec_ctx->pix_fmt, dst_w, dst_h, stream->time_base.num, stream->time_base.den);

  uint8_t *dst_data[4] = {nullptr, nullptr, nullptr, nullptr};
  int dst_linesize[4] = {0, 0, 0, 0};
  av_image_fill_arrays(dst_data, dst_linesize, dst_i420.data(), AV_PIX_FMT_YUV420P,
                       dst_w, dst_h, 1);

  bool clock_started = false;
  int64_t base_wall_us = 0;
  int64_t base_pts_us = 0;
  uint64_t decoded = 0;
  int64_t fps_window_us = MonotonicUs();
  uint64_t fps_frames = 0;

  while (!g_stop.load(std::memory_order_acquire)) {
    const int read_rc = av_read_frame(fmt, pkt);
    if (read_rc < 0) {
      avcodec_send_packet(codec_ctx, nullptr);
    } else if (pkt->stream_index == stream_index) {
      if (avcodec_send_packet(codec_ctx, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
    }
    av_packet_unref(pkt);

    while (!g_stop.load(std::memory_order_acquire)) {
      const int rc = avcodec_receive_frame(codec_ctx, frame);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) {
        LOGW("FFmpegPlayer: avcodec_receive_frame rc=%d", rc);
        break;
      }

      sws = sws_getCachedContext(sws, frame->width, frame->height,
                                 static_cast<AVPixelFormat>(frame->format), dst_w, dst_h,
                                 AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
                                 nullptr, nullptr, nullptr);
      if (sws == nullptr) {
        LOGE("FFmpegPlayer: sws_getCachedContext failed");
        av_frame_unref(frame);
        continue;
      }
      const int scaled = sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                                   dst_data, dst_linesize);
      if (scaled != dst_h) {
        LOGW("FFmpegPlayer: sws_scale returned %d expected %d", scaled, dst_h);
        av_frame_unref(frame);
        continue;
      }
      const int64_t pts_us = FramePtsUs(frame, stream->time_base);
      PaceFrame(pts_us, &clock_started, &base_wall_us, &base_pts_us);
      if (!WriteFrame(&ring, dst_i420.data(), dst_i420.size(), pts_us)) {
        LOGE("FFmpegPlayer: WriteFrame failed");
      }
      decoded += 1;
      fps_frames += 1;
      if (decoded <= 5 || (decoded % 120) == 0) {
        LOGI("FFmpegPlayer: wrote source frame #%llu gen=%llu slot=%u out=%dx%d pts=%lld",
             static_cast<unsigned long long>(decoded),
             static_cast<unsigned long long>(ring.next_generation),
             (ring.next_slot + awesomecam::kSharedMemoryRingSlotCount - 1) %
                 awesomecam::kSharedMemoryRingSlotCount,
             dst_w, dst_h, static_cast<long long>(pts_us));
      }
      const int64_t now_us = MonotonicUs();
      if (now_us - fps_window_us >= 2000000LL) {
        const double fps = static_cast<double>(fps_frames) * 1000000.0 /
                           static_cast<double>(now_us - fps_window_us);
        LOGI("FFmpegPlayer: decoded FPS fps=%.1f frames=%llu latestGen=%llu",
             fps, static_cast<unsigned long long>(fps_frames),
             static_cast<unsigned long long>(ring.next_generation));
        fps_window_us = now_us;
        fps_frames = 0;
      }
      av_frame_unref(frame);
    }
    if (read_rc < 0) {
      ok = true;
      break;
    }
  }

  cleanup();
  return ok;
}

}  // namespace

int main(int argc, char **argv) {
  signal(SIGTERM, SignalHandler);
  signal(SIGINT, SignalHandler);
  av_log_set_level(AV_LOG_ERROR);

  Options opt = ParseOptions(argc, argv);
  WritePidFile(opt.pidfile);

  BinderClient binder{};
  if (!ConnectService(&binder)) {
    if (!opt.pidfile.empty()) unlink(opt.pidfile.c_str());
    return 2;
  }

  uint64_t loops = 0;
  do {
    loops += 1;
    const bool ok = DecodeOnce(opt, &binder);
    if (!ok && !g_stop.load(std::memory_order_acquire)) {
      LOGW("FFmpegPlayer: decode loop failed; retry in 500ms");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else if (opt.loop && !g_stop.load(std::memory_order_acquire)) {
      LOGI("FFmpegPlayer: loop EOF reached, restarting input loops=%llu",
           static_cast<unsigned long long>(loops));
    }
  } while (opt.loop && !g_stop.load(std::memory_order_acquire));

  ClearService(&binder);
  if (!opt.pidfile.empty()) unlink(opt.pidfile.c_str());
  LOGI("FFmpegPlayer: exit");
  return 0;
}
