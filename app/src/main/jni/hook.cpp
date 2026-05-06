#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "libyuv_runtime.h"
#include "shadowhook.h"
#include "video2camera_service.h"
#include "video2camera_ipc.h"
#include "ready_frame_cache.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void *g_orig_camera3device_create_stream = nullptr;
extern "C" void *g_orig_return_buffer_checked_locked = nullptr;
extern "C" void *g_orig_queue_buffer_to_consumer = nullptr;
extern "C" void *g_orig_surface_hook_queue_buffer = nullptr;
extern "C" void *g_orig_surface_set_usage = nullptr;

namespace {

namespace android {
template <typename T>
class sp;
class Fence;
namespace camera3 {
struct camera_stream {
  int stream_type;
  uint32_t width;
  uint32_t height;
  int format;
  uint64_t usage;
  uint32_t max_buffers;
  void *priv;
  int32_t data_space;
};

struct camera_stream_buffer {
  camera_stream *stream;
  void *buffer;
  int status;
  int acquire_fence;
  int release_fence;
};
}  // namespace camera3
}  // namespace android

struct android_ycbcr {
  void *y;
  void *cb;
  void *cr;
  size_t ystride;
  size_t cstride;
  size_t chroma_step;
  uint32_t reserved[8];
};


struct AndroidNativeBasePrefix {
  int32_t magic;
  int32_t version;
  void *reserved[4];
  void (*inc_ref)(void *);
  void (*dec_ref)(void *);
};

struct ANativeWindowBufferPrefix {
  AndroidNativeBasePrefix common;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  int32_t usage_deprecated;
  uintptr_t layer_count;
  void *reserved[1];
  void *handle;
  uint64_t usage;
};

static_assert(offsetof(ANativeWindowBufferPrefix, handle) == 0x60,
              "ANativeWindowBuffer handle offset changed");

constexpr const char *kTargetModuleBasename = "cameraserver";

constexpr int kHalPixelFormatBlob = 0x21;
constexpr int kHalPixelFormatImplementationDefined = 0x22;
constexpr int kHalPixelFormatYcbcr420888 = 0x23;
constexpr int kHalPixelFormatRaw10 = 0x25;
constexpr int32_t kDataspaceJfif = 0x8c20000;
constexpr bool kForceImplementationDefinedPreviewToYuv = true;

constexpr const char *kCamera3DeviceCreateStreamSymbol =
    "android::Camera3Device::createStream(std::vector<SurfaceHolder> const&, ...)";
constexpr const char *kReturnBufferCheckedLockedSymbol =
    "android::camera3::Camera3OutputStream::returnBufferCheckedLocked";
constexpr const char *kQueueBufferToConsumerSymbol =
    "android::camera3::Camera3OutputStream::queueBufferToConsumer";
constexpr const char *kCamera3DeviceCreateStreamMangledSymbol =
    "_ZN7android13Camera3Device12createStreamERKNSt3__16vectorINS_13SurfaceHolderENS1_9allocatorIS3_EEEEbjjiiiPiRKNS1_12basic_stringIcNS1_11char_traitsIcEENS4_IcEEEERKNS1_13unordered_setIiNS1_4hashIiEENS1_8equal_toIiEENS4_IiEEEEPNS2_IiSM_EEibbmlliib";
constexpr const char *kReturnBufferCheckedLockedMangledSymbol =
    "_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLockedERKNS0_20camera_stream_bufferEllbiRKNSt3__16vectorImNS5_9allocatorImEEEEPNS_2spINS_5FenceEEE";
constexpr const char *kQueueBufferToConsumerMangledSymbol =
    "_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS8_9allocatorImEEEE";

constexpr uint32_t kGraphicBufferCpuLockUsage = 0x33;
constexpr uint64_t kGrallocUsageCpuReadWriteOften = 0x33;
constexpr uintptr_t kAnwHandleOffset = 0x60;
constexpr uint64_t kNsPerSecond = 1000000000ULL;
constexpr int kDefaultFenceWaitTimeoutMs = 1000;
constexpr int kSurfaceFenceWaitTimeoutMs = 3;  // Surface hot path: skip, do not stall, after a short wait.
constexpr uint64_t kWriteModeRefreshIntervalNs = kNsPerSecond;
constexpr size_t kMaxBufferLockCacheEntries = 128;
constexpr const char *kWriteAllFlagPath = "/data/camera/awesomecam_write_all";
constexpr const char *kWriteCamera3FlagPath = "/data/camera/awesomecam_write_camera3";

enum class WriteMode : int {
  kSurfaceOnly = 0,
  kAll = 1,
  kCamera3Only = 2,
};

enum class BufferLockPath : int {
  kUnknown = 0,
  kYCbCr = 1,
  kAhbPlanes = 2,
  kRaw = 3,
};

struct StreamRecord {
  int stream_id;
  uint32_t width;
  uint32_t height;
  int format;
  int32_t data_space;
  uint64_t consumer_usage;
  int stream_set_id;
  bool deferred;
  bool shared;
  bool multi_resolution;
  int timestamp_base;
  int32_t color_space;
  bool use_readout_timestamp;
  uint64_t return_buffer_log_count;
};

struct BufferLockCacheEntry {
  uintptr_t anw_buffer = 0;
  int width = 0;
  int height = 0;
  int format = 0;
  BufferLockPath preferred = BufferLockPath::kUnknown;
  bool ycbcr_failed = false;
  bool ahb_failed = false;
  bool raw_failed = false;
  uint64_t updates = 0;
};

struct CachedWriteResult {
  BufferLockPath path = BufferLockPath::kUnknown;
  android_ycbcr layout{};
  int raw_bpp = 0;
  int raw_stride = 0;
  bool raw_nv21 = false;
};

struct GraphicBufferSpRet {
  void *ptr;
  void *pad1;
  void *pad2;
};

using GraphicBufferFromFn = GraphicBufferSpRet (*)(void *anw_buffer);
using GraphicBufferLockYCbCrFn = int (*)(void *graphic_buffer, unsigned int usage,
                                         android_ycbcr *layout);
using GraphicBufferLockFn = int (*)(void *graphic_buffer, unsigned int usage,
                                    void **vaddr, int *bytes_per_pixel,
                                    int *bytes_per_stride);
using GraphicBufferUnlockFn = int (*)(void *graphic_buffer);
using RefBaseDecStrongFn = void (*)(void *self, const void *id);

struct AHardwareBufferRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

struct AHardwareBufferPlane {
  void *data;
  uint32_t pixelStride;
  uint32_t rowStride;
};

struct AHardwareBufferPlanes {
  uint32_t planeCount;
  AHardwareBufferPlane planes[4];
};

using AHardwareBufferFromGraphicBufferFn = void *(*)(void *graphic_buffer);
using AHardwareBufferLockPlanesFn = int (*)(void *buffer, uint64_t usage,
                                            int32_t fence,
                                            const AHardwareBufferRect *rect,
                                            AHardwareBufferPlanes *out_planes);
using AHardwareBufferUnlockFn = int (*)(void *buffer, int32_t *fence);

struct GraphicBufferApi {
  std::atomic<bool> resolved{false};
  GraphicBufferFromFn from = nullptr;
  GraphicBufferLockYCbCrFn lock_ycbcr = nullptr;
  GraphicBufferLockFn lock = nullptr;
  GraphicBufferUnlockFn unlock = nullptr;
  RefBaseDecStrongFn dec_strong = nullptr;
  AHardwareBufferFromGraphicBufferFn ahb_from_graphic_buffer = nullptr;
  AHardwareBufferLockPlanesFn ahb_lock_planes = nullptr;
  AHardwareBufferUnlockFn ahb_unlock = nullptr;
};

std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_create_stream_hit_count{0};
std::atomic<uint64_t> g_return_buffer_hit_count{0};
std::atomic<uint64_t> g_queue_buffer_to_consumer_hit_count{0};
std::atomic<uint64_t> g_queue_replaced_frame_count{0};
std::atomic<uint64_t> g_surface_queue_buffer_hit_count{0};
std::atomic<uint64_t> g_surface_set_usage_hit_count{0};
std::atomic<uint64_t> g_replaced_frame_count{0};
std::atomic<uint64_t> g_lock_ycbcr_fail_count{0};
std::atomic<uint64_t> g_raw_lock_fail_count{0};
std::atomic<uint64_t> g_raw_lock_success_count{0};
std::atomic<uint64_t> g_fence_wait_count{0};
std::atomic<uint64_t> g_fence_wait_timeout_count{0};
std::atomic<uint64_t> g_ahb_lock_fail_count{0};
std::atomic<uint64_t> g_ahb_lock_success_count{0};
std::atomic<int> g_write_mode{static_cast<int>(WriteMode::kSurfaceOnly)};
std::atomic<uint64_t> g_write_mode_last_refresh_ns{0};

void *g_camera3device_create_stream_stub = nullptr;
void *g_return_buffer_checked_locked_stub = nullptr;
void *g_queue_buffer_to_consumer_stub = nullptr;
void *g_surface_hook_queue_buffer_stub = nullptr;
void *g_surface_set_usage_stub = nullptr;

std::mutex g_stream_records_mutex;
std::mutex g_buffer_lock_cache_mutex;
std::vector<StreamRecord> g_stream_records;
std::vector<BufferLockCacheEntry> g_buffer_lock_cache;
GraphicBufferApi g_graphic_buffer_api;
void *g_self_handle = nullptr;

uint64_t monotonic_time_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond +
         static_cast<uint64_t>(ts.tv_nsec);
}

double ns_to_ms(uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
}

const char *write_mode_name(WriteMode mode) {
  switch (mode) {
    case WriteMode::kSurfaceOnly:
      return "surfaceOnly";
    case WriteMode::kAll:
      return "writeAll";
    case WriteMode::kCamera3Only:
      return "camera3Only";
  }
  return "unknown";
}

WriteMode write_mode_from_int(int value) {
  switch (value) {
    case static_cast<int>(WriteMode::kAll):
      return WriteMode::kAll;
    case static_cast<int>(WriteMode::kCamera3Only):
      return WriteMode::kCamera3Only;
    case static_cast<int>(WriteMode::kSurfaceOnly):
    default:
      return WriteMode::kSurfaceOnly;
  }
}

WriteMode detect_write_mode_from_flags() {
  if (access(kWriteAllFlagPath, F_OK) == 0) {
    return WriteMode::kAll;
  }
  if (access(kWriteCamera3FlagPath, F_OK) == 0) {
    return WriteMode::kCamera3Only;
  }
  return WriteMode::kSurfaceOnly;
}

WriteMode current_write_mode() {
  const uint64_t now_ns = monotonic_time_ns();
  uint64_t last_ns = g_write_mode_last_refresh_ns.load(std::memory_order_acquire);
  if (last_ns == 0 || now_ns - last_ns >= kWriteModeRefreshIntervalNs) {
    if (g_write_mode_last_refresh_ns.compare_exchange_strong(
            last_ns, now_ns, std::memory_order_acq_rel, std::memory_order_acquire)) {
      const WriteMode detected = detect_write_mode_from_flags();
      const WriteMode previous = write_mode_from_int(
          g_write_mode.exchange(static_cast<int>(detected), std::memory_order_acq_rel));
      if (previous != detected) {
        LOGI("write mode changed: %s -> %s (all=%s camera3=%s)",
             write_mode_name(previous), write_mode_name(detected),
             kWriteAllFlagPath, kWriteCamera3FlagPath);
      }
      return detected;
    }
  }
  return write_mode_from_int(g_write_mode.load(std::memory_order_acquire));
}

bool write_mode_allows_surface(WriteMode mode) {
  return mode == WriteMode::kSurfaceOnly || mode == WriteMode::kAll;
}

bool write_mode_allows_camera3(WriteMode mode) {
  return mode == WriteMode::kAll || mode == WriteMode::kCamera3Only;
}

const char *buffer_lock_path_name(BufferLockPath path) {
  switch (path) {
    case BufferLockPath::kYCbCr:
      return "lockYCbCr";
    case BufferLockPath::kAhbPlanes:
      return "AHardwareBuffer_lockPlanes";
    case BufferLockPath::kRaw:
      return "GraphicBuffer::lock";
    case BufferLockPath::kUnknown:
    default:
      return "unknown";
  }
}

bool same_buffer_lock_key(const BufferLockCacheEntry &entry, uintptr_t anw_buffer,
                          int width, int height, int format) {
  return entry.anw_buffer == anw_buffer && entry.width == width &&
         entry.height == height && entry.format == format;
}

BufferLockCacheEntry lookup_buffer_lock_cache(uintptr_t anw_buffer, int width,
                                              int height, int format) {
  if (anw_buffer == 0 || width <= 0 || height <= 0) return BufferLockCacheEntry{};
  std::lock_guard<std::mutex> lock(g_buffer_lock_cache_mutex);
  for (const auto &entry : g_buffer_lock_cache) {
    if (same_buffer_lock_key(entry, anw_buffer, width, height, format)) {
      return entry;
    }
  }
  return BufferLockCacheEntry{};
}

bool buffer_lock_path_failed(const BufferLockCacheEntry &entry, BufferLockPath path) {
  switch (path) {
    case BufferLockPath::kYCbCr:
      return entry.ycbcr_failed;
    case BufferLockPath::kAhbPlanes:
      return entry.ahb_failed;
    case BufferLockPath::kRaw:
      return entry.raw_failed;
    case BufferLockPath::kUnknown:
    default:
      return false;
  }
}

void set_buffer_lock_path_failed(BufferLockCacheEntry *entry, BufferLockPath path,
                                 bool failed) {
  if (entry == nullptr) return;
  switch (path) {
    case BufferLockPath::kYCbCr:
      entry->ycbcr_failed = failed;
      break;
    case BufferLockPath::kAhbPlanes:
      entry->ahb_failed = failed;
      break;
    case BufferLockPath::kRaw:
      entry->raw_failed = failed;
      break;
    case BufferLockPath::kUnknown:
    default:
      break;
  }
}

void record_buffer_lock_path(uintptr_t anw_buffer, int width, int height, int format,
                             BufferLockPath path, bool success, const char *where) {
  if (anw_buffer == 0 || width <= 0 || height <= 0 || path == BufferLockPath::kUnknown) {
    return;
  }

  BufferLockCacheEntry snapshot{};
  bool added = false;
  {
    std::lock_guard<std::mutex> lock(g_buffer_lock_cache_mutex);
    auto it = std::find_if(g_buffer_lock_cache.begin(), g_buffer_lock_cache.end(),
                           [&](const BufferLockCacheEntry &entry) {
                             return same_buffer_lock_key(entry, anw_buffer, width,
                                                         height, format);
                           });
    if (it == g_buffer_lock_cache.end()) {
      if (g_buffer_lock_cache.size() >= kMaxBufferLockCacheEntries) {
        g_buffer_lock_cache.erase(g_buffer_lock_cache.begin());
      }
      BufferLockCacheEntry entry{};
      entry.anw_buffer = anw_buffer;
      entry.width = width;
      entry.height = height;
      entry.format = format;
      g_buffer_lock_cache.push_back(entry);
      it = g_buffer_lock_cache.end() - 1;
      added = true;
    }

    it->updates += 1;
    if (success) {
      it->preferred = path;
      set_buffer_lock_path_failed(&*it, path, false);
    } else {
      set_buffer_lock_path_failed(&*it, path, true);
      if (it->preferred == path) it->preferred = BufferLockPath::kUnknown;
    }
    snapshot = *it;
  }

  if (success && (added || snapshot.updates <= 3 || (snapshot.updates % 120) == 0)) {
    LOGI("%s buffer lock cache %dx%d fmt=%#x anw=%p preferred=%s failed[y=%d ahb=%d raw=%d]",
         where != nullptr ? where : "replace", width, height, format,
         reinterpret_cast<void *>(anw_buffer), buffer_lock_path_name(snapshot.preferred),
         snapshot.ycbcr_failed ? 1 : 0, snapshot.ahb_failed ? 1 : 0,
         snapshot.raw_failed ? 1 : 0);
  }
}

void append_unique_lock_path(std::vector<BufferLockPath> *order, BufferLockPath path) {
  if (order == nullptr || path == BufferLockPath::kUnknown) return;
  for (BufferLockPath existing : *order) {
    if (existing == path) return;
  }
  order->push_back(path);
}

bool resolve_graphic_buffer_api() {
  if (g_graphic_buffer_api.resolved.load(std::memory_order_acquire)) {
    return g_graphic_buffer_api.from != nullptr && g_graphic_buffer_api.lock_ycbcr != nullptr &&
           g_graphic_buffer_api.lock != nullptr && g_graphic_buffer_api.unlock != nullptr &&
           g_graphic_buffer_api.dec_strong != nullptr;
  }

  g_graphic_buffer_api.from = reinterpret_cast<GraphicBufferFromFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer4fromEP19ANativeWindowBuffer"));
  g_graphic_buffer_api.lock_ycbcr = reinterpret_cast<GraphicBufferLockYCbCrFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer9lockYCbCrEjP13android_ycbcr"));
  g_graphic_buffer_api.lock = reinterpret_cast<GraphicBufferLockFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer4lockEjPPvPiS3_"));
  g_graphic_buffer_api.unlock = reinterpret_cast<GraphicBufferUnlockFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer6unlockEv"));
  g_graphic_buffer_api.dec_strong = reinterpret_cast<RefBaseDecStrongFn>(
      dlsym(RTLD_DEFAULT, "_ZNK7android7RefBase9decStrongEPKv"));
  g_graphic_buffer_api.ahb_from_graphic_buffer =
      reinterpret_cast<AHardwareBufferFromGraphicBufferFn>(
          dlsym(RTLD_DEFAULT, "_ZN7android34AHardwareBuffer_from_GraphicBufferEPNS_13GraphicBufferE"));
  g_graphic_buffer_api.ahb_lock_planes =
      reinterpret_cast<AHardwareBufferLockPlanesFn>(
          dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockPlanes"));
  g_graphic_buffer_api.ahb_unlock =
      reinterpret_cast<AHardwareBufferUnlockFn>(
          dlsym(RTLD_DEFAULT, "AHardwareBuffer_unlock"));
  g_graphic_buffer_api.resolved.store(true, std::memory_order_release);

  const bool ok = g_graphic_buffer_api.from != nullptr &&
                  g_graphic_buffer_api.lock_ycbcr != nullptr &&
                  g_graphic_buffer_api.lock != nullptr &&
                  g_graphic_buffer_api.unlock != nullptr &&
                  g_graphic_buffer_api.dec_strong != nullptr;
  if (!ok) {
    LOGE("Failed to resolve GraphicBuffer symbols from=%p lockYCbCr=%p lock=%p unlock=%p decStrong=%p ahbFrom=%p ahbLockPlanes=%p ahbUnlock=%p",
         reinterpret_cast<void *>(g_graphic_buffer_api.from),
         reinterpret_cast<void *>(g_graphic_buffer_api.lock_ycbcr),
         reinterpret_cast<void *>(g_graphic_buffer_api.lock),
         reinterpret_cast<void *>(g_graphic_buffer_api.unlock),
         reinterpret_cast<void *>(g_graphic_buffer_api.dec_strong),
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_from_graphic_buffer),
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_lock_planes),
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_unlock));
  } else {
    LOGI("GraphicBuffer API resolved ahbFrom=%p ahbLockPlanes=%p ahbUnlock=%p",
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_from_graphic_buffer),
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_lock_planes),
         reinterpret_cast<void *>(g_graphic_buffer_api.ahb_unlock));
  }
  return ok;
}

void register_video_target_if_supported(uint32_t width, uint32_t height, int format);

void remember_stream_record(int stream_id, uint32_t width, uint32_t height, int format,
                            int32_t data_space, uint64_t consumer_usage,
                            int stream_set_id, bool deferred, bool shared,
                            bool multi_resolution, int timestamp_base,
                            int32_t color_space, bool use_readout_timestamp) {
  if (stream_id < 0) return;

  std::lock_guard<std::mutex> lock(g_stream_records_mutex);
  for (auto &record : g_stream_records) {
    if (record.stream_id == stream_id) {
      record.width = width;
      record.height = height;
      record.format = format;
      record.data_space = data_space;
      record.consumer_usage = consumer_usage;
      record.stream_set_id = stream_set_id;
      record.deferred = deferred;
      record.shared = shared;
      record.multi_resolution = multi_resolution;
      record.timestamp_base = timestamp_base;
      record.color_space = color_space;
      record.use_readout_timestamp = use_readout_timestamp;
      return;
    }
  }

  g_stream_records.push_back(StreamRecord{stream_id,
                                          width,
                                          height,
                                          format,
                                          data_space,
                                          consumer_usage,
                                          stream_set_id,
                                          deferred,
                                          shared,
                                          multi_resolution,
                                          timestamp_base,
                                          color_space,
                                          use_readout_timestamp,
                                          0});
}

bool should_force_implementation_defined_preview_to_yuv(uint32_t width,
                                                        uint32_t height,
                                                        int format,
                                                        int32_t data_space,
                                                        uint64_t consumer_usage) {
  if (!kForceImplementationDefinedPreviewToYuv) return false;
  if (format != kHalPixelFormatImplementationDefined) return false;
  if (width == 0 || height == 0) return false;

  // Still/capture encoded paths normally use BLOB/RAW.  Do not touch JFIF-like
  // implementation-defined streams if any vendor stack uses them for capture.
  if (data_space == kDataspaceJfif) return false;

  // Preview/video consumers often request IMPLEMENTATION_DEFINED private GPU
  // buffers. Those return -ENOSYS from lockYCbCr, so force the camera stream
  // config to flexible YUV while leaving the consumer Surface itself alone.
  (void)consumer_usage;
  return true;
}

void register_video_target_if_supported(uint32_t width, uint32_t height, int format) {
  if (width == 0 || height == 0) return;
  if (format != kHalPixelFormatImplementationDefined &&
      format != kHalPixelFormatYcbcr420888) {
    return;
  }
  awesomecam::UpdateVideo2CameraTarget(static_cast<int32_t>(width),
                                       static_cast<int32_t>(height), format);
  awesomecam::RegisterReadyFrameTarget(static_cast<int32_t>(width),
                                       static_cast<int32_t>(height));
}

bool find_stream_record_by_props(uint32_t width, uint32_t height, int format,
                                 int32_t data_space, StreamRecord *out,
                                 uint64_t *log_count_after_increment) {
  std::lock_guard<std::mutex> lock(g_stream_records_mutex);

  auto exact_match = [&](StreamRecord &record) {
    return record.width == width && record.height == height && record.format == format &&
           record.data_space == data_space;
  };

  auto soft_match = [&](StreamRecord &record) {
    return record.width == width && record.height == height &&
           ((record.format == format) || (record.format == 0x22 && format == 0x23));
  };

  for (auto &record : g_stream_records) {
    if (!exact_match(record)) continue;
    record.return_buffer_log_count += 1;
    if (out != nullptr) *out = record;
    if (log_count_after_increment != nullptr) {
      *log_count_after_increment = record.return_buffer_log_count;
    }
    return true;
  }

  for (auto &record : g_stream_records) {
    if (!soft_match(record)) continue;
    record.return_buffer_log_count += 1;
    if (record.format == 0x22 && format == 0x23) {
      record.format = format;
      record.data_space = data_space;
    }
    if (out != nullptr) *out = record;
    if (log_count_after_increment != nullptr) {
      *log_count_after_increment = record.return_buffer_log_count;
    }
    return true;
  }

  return false;
}

size_t chroma_width_for(int width) { return static_cast<size_t>((width + 1) / 2); }
size_t chroma_height_for(int height) { return static_cast<size_t>((height + 1) / 2); }

void copy_plane_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src, int src_stride,
                     int row_width, int rows) {
  for (int y = 0; y < rows; ++y) {
    memcpy(dst + y * dst_stride, src + y * src_stride, static_cast<size_t>(row_width));
  }
}

bool write_i420_to_ycbcr(const uint8_t *scaled_i420, size_t scaled_i420_size,
                         int width, int height, const android_ycbcr &layout) {
  if (width <= 0 || height <= 0 || layout.y == nullptr || layout.cb == nullptr ||
      layout.cr == nullptr) {
    return false;
  }

  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_width = chroma_width_for(width);
  const size_t chroma_height = chroma_height_for(height);
  const size_t chroma_size = chroma_width * chroma_height;
  if (scaled_i420 == nullptr || scaled_i420_size < y_size + 2 * chroma_size) return false;

  const uint8_t *src_y = scaled_i420;
  const uint8_t *src_u = src_y + y_size;
  const uint8_t *src_v = src_u + chroma_size;

  auto *dst_y = static_cast<uint8_t *>(layout.y);
  auto *dst_cb = static_cast<uint8_t *>(layout.cb);
  auto *dst_cr = static_cast<uint8_t *>(layout.cr);

  if (layout.chroma_step == 1) {
    if (awesomecam::LibYuvI420Copy(src_y, width, src_u, static_cast<int>(chroma_width),
                                   src_v, static_cast<int>(chroma_width), dst_y,
                                   static_cast<int>(layout.ystride), dst_cb,
                                   static_cast<int>(layout.cstride), dst_cr,
                                   static_cast<int>(layout.cstride), width, height)) {
      return true;
    }
    copy_plane_rows(dst_y, layout.ystride, src_y, width, width, height);
    copy_plane_rows(dst_cb, layout.cstride, src_u, static_cast<int>(chroma_width),
                    static_cast<int>(chroma_width), static_cast<int>(chroma_height));
    copy_plane_rows(dst_cr, layout.cstride, src_v, static_cast<int>(chroma_width),
                    static_cast<int>(chroma_width), static_cast<int>(chroma_height));
    return true;
  }

  if (layout.chroma_step == 2) {
    const bool nv12_layout = dst_cb < dst_cr;
    uint8_t *dst_uv = nv12_layout ? dst_cb : dst_cr;
    const bool libyuv_ok =
        nv12_layout
            ? awesomecam::LibYuvI420ToNV12(
                  src_y, width, src_u, static_cast<int>(chroma_width), src_v,
                  static_cast<int>(chroma_width), dst_y,
                  static_cast<int>(layout.ystride), dst_uv,
                  static_cast<int>(layout.cstride), width, height)
            : awesomecam::LibYuvI420ToNV21(
                  src_y, width, src_u, static_cast<int>(chroma_width), src_v,
                  static_cast<int>(chroma_width), dst_y,
                  static_cast<int>(layout.ystride), dst_uv,
                  static_cast<int>(layout.cstride), width, height);
    if (libyuv_ok) return true;

    copy_plane_rows(dst_y, layout.ystride, src_y, width, width, height);
    for (size_t y = 0; y < chroma_height; ++y) {
      const uint8_t *u_row = src_u + y * chroma_width;
      const uint8_t *v_row = src_v + y * chroma_width;
      uint8_t *dst_row = dst_uv + y * layout.cstride;
      for (size_t x = 0; x < chroma_width; ++x) {
        if (nv12_layout) {
          dst_row[x * 2 + 0] = u_row[x];
          dst_row[x * 2 + 1] = v_row[x];
        } else {
          dst_row[x * 2 + 0] = v_row[x];
          dst_row[x * 2 + 1] = u_row[x];
        }
      }
    }
    return true;
  }

  copy_plane_rows(dst_y, layout.ystride, src_y, width, width, height);
  for (size_t y = 0; y < chroma_height; ++y) {
    uint8_t *cb_row = dst_cb + y * layout.cstride;
    uint8_t *cr_row = dst_cr + y * layout.cstride;
    const uint8_t *u_row = src_u + y * chroma_width;
    const uint8_t *v_row = src_v + y * chroma_width;
    for (size_t x = 0; x < chroma_width; ++x) {
      cb_row[x * layout.chroma_step] = u_row[x];
      cr_row[x * layout.chroma_step] = v_row[x];
    }
  }

  return true;
}

bool prefer_raw_nv21_output() {
  return access("/data/camera/awesomecam_use_nv21", F_OK) == 0;
}

bool wait_fence_if_valid(int fence_fd, const char *where, int timeout_ms) {
  if (fence_fd < 0) return true;

  const int poll_timeout_ms = timeout_ms >= 0 ? timeout_ms : kDefaultFenceWaitTimeoutMs;
  const uint64_t start_ns = monotonic_time_ns();
  pollfd pfd{};
  pfd.fd = fence_fd;
  pfd.events = POLLIN;

  int rc = -1;
  do {
    rc = poll(&pfd, 1, poll_timeout_ms);
  } while (rc < 0 && errno == EINTR);

  const uint64_t done_ns = monotonic_time_ns();
  const uint64_t count =
      g_fence_wait_count.fetch_add(1, std::memory_order_relaxed) + 1;

  if (rc == 0) {
    const uint64_t timeout_count =
        g_fence_wait_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (timeout_count <= 20 || (timeout_count % 120) == 0) {
      LOGW("%s fence wait timeout #%llu fd=%d timeout=%dms waited=%.3fms",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(timeout_count), fence_fd, poll_timeout_ms,
           ns_to_ms(done_ns - start_ns));
    }
    return false;
  }

  if (rc < 0) {
    if (count <= 20 || (count % 120) == 0) {
      LOGW("%s fence wait failed fd=%d errno=%d (%s)",
           where != nullptr ? where : "replace", fence_fd, errno, strerror(errno));
    }
    return false;
  }

  if (count <= 20 || (count % 120) == 0) {
    LOGI("%s fence signaled #%llu fd=%d waited=%.3fms revents=%#x",
         where != nullptr ? where : "replace",
         static_cast<unsigned long long>(count), fence_fd,
         ns_to_ms(done_ns - start_ns), pfd.revents);
  }
  return true;
}

bool write_i420_to_contiguous_nv(const uint8_t *scaled_i420, size_t scaled_i420_size,
                                 int width, int height, void *vaddr,
                                 int stride_bytes, bool nv21) {
  if (width <= 0 || height <= 0 || vaddr == nullptr) return false;
  if (stride_bytes <= 0) stride_bytes = width;
  if (stride_bytes < width) return false;

  const int chroma_width = static_cast<int>(chroma_width_for(width));
  const int chroma_height = static_cast<int>(chroma_height_for(height));
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_size = static_cast<size_t>(chroma_width) * chroma_height;
  if (scaled_i420 == nullptr || scaled_i420_size < y_size + 2 * chroma_size) return false;

  const uint8_t *src_y = scaled_i420;
  const uint8_t *src_u = src_y + y_size;
  const uint8_t *src_v = src_u + chroma_size;
  auto *dst_y = static_cast<uint8_t *>(vaddr);
  auto *dst_uv = dst_y + static_cast<size_t>(stride_bytes) * height;

  const bool libyuv_ok = nv21
      ? awesomecam::LibYuvI420ToNV21(src_y, width, src_u, chroma_width, src_v,
                                     chroma_width, dst_y, stride_bytes, dst_uv,
                                     stride_bytes, width, height)
      : awesomecam::LibYuvI420ToNV12(src_y, width, src_u, chroma_width, src_v,
                                     chroma_width, dst_y, stride_bytes, dst_uv,
                                     stride_bytes, width, height);
  if (libyuv_ok) return true;

  copy_plane_rows(dst_y, static_cast<size_t>(stride_bytes), src_y, width, width, height);
  for (int y = 0; y < chroma_height; ++y) {
    const uint8_t *u_row = src_u + static_cast<size_t>(y) * chroma_width;
    const uint8_t *v_row = src_v + static_cast<size_t>(y) * chroma_width;
    uint8_t *dst_row = dst_uv + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < chroma_width; ++x) {
      if (nv21) {
        dst_row[x * 2 + 0] = v_row[x];
        dst_row[x * 2 + 1] = u_row[x];
      } else {
        dst_row[x * 2 + 0] = u_row[x];
        dst_row[x * 2 + 1] = v_row[x];
      }
    }
  }
  return true;
}

awesomecam::ReadyFrameLayout ready_layout_for_ycbcr(const android_ycbcr &layout) {
  if (layout.chroma_step == 1) return awesomecam::kReadyFrameLayoutI420;
  if (layout.chroma_step == 2 && layout.cb != nullptr && layout.cr != nullptr) {
    return static_cast<uint8_t *>(layout.cb) < static_cast<uint8_t *>(layout.cr)
               ? awesomecam::kReadyFrameLayoutNV12
               : awesomecam::kReadyFrameLayoutNV21;
  }
  return awesomecam::kReadyFrameLayoutI420;
}

size_t ready_layout_chroma_row_bytes(int width, awesomecam::ReadyFrameLayout layout) {
  if (layout == awesomecam::kReadyFrameLayoutI420) return chroma_width_for(width);
  return chroma_width_for(width) * 2;
}

bool write_ready_nv_to_ycbcr(const awesomecam::ReadyI420Frame &frame,
                             const android_ycbcr &layout) {
  if (frame.layout != awesomecam::kReadyFrameLayoutNV12 &&
      frame.layout != awesomecam::kReadyFrameLayoutNV21) {
    return false;
  }
  if (layout.y == nullptr || layout.cb == nullptr || layout.cr == nullptr ||
      layout.chroma_step != 2 || frame.width <= 0 || frame.height <= 0) {
    return false;
  }
  const auto wanted_layout = static_cast<awesomecam::ReadyFrameLayout>(frame.layout);
  if (ready_layout_for_ycbcr(layout) != wanted_layout) return false;

  const int width = frame.width;
  const int height = frame.height;
  const int y_stride = frame.y_stride > 0 ? frame.y_stride : width;
  const int uv_stride = frame.c_stride > 0
                            ? frame.c_stride
                            : static_cast<int>(ready_layout_chroma_row_bytes(width, wanted_layout));
  const size_t y_size = static_cast<size_t>(y_stride) * height;
  const size_t uv_row_bytes = chroma_width_for(width) * 2;
  const size_t uv_rows = chroma_height_for(height);
  if (frame.bytes.size() < y_size + static_cast<size_t>(uv_stride) * uv_rows) {
    return false;
  }

  auto *dst_y = static_cast<uint8_t *>(layout.y);
  auto *dst_uv = wanted_layout == awesomecam::kReadyFrameLayoutNV12
                     ? static_cast<uint8_t *>(layout.cb)
                     : static_cast<uint8_t *>(layout.cr);
  const uint8_t *src_y = frame.bytes.data();
  const uint8_t *src_uv = src_y + y_size;
  copy_plane_rows(dst_y, layout.ystride, src_y, y_stride, width, height);
  copy_plane_rows(dst_uv, layout.cstride, src_uv, uv_stride,
                  static_cast<int>(uv_row_bytes), static_cast<int>(uv_rows));
  return true;
}

bool write_ready_nv_to_contiguous_nv(const awesomecam::ReadyI420Frame &frame,
                                     void *vaddr, int stride_bytes,
                                     bool nv21) {
  const auto wanted_layout =
      nv21 ? awesomecam::kReadyFrameLayoutNV21 : awesomecam::kReadyFrameLayoutNV12;
  if (frame.layout != wanted_layout || vaddr == nullptr || frame.width <= 0 ||
      frame.height <= 0) {
    return false;
  }
  const int width = frame.width;
  const int height = frame.height;
  if (stride_bytes <= 0) stride_bytes = width;
  const int y_stride = frame.y_stride > 0 ? frame.y_stride : width;
  const int uv_stride = frame.c_stride > 0
                            ? frame.c_stride
                            : static_cast<int>(ready_layout_chroma_row_bytes(width, wanted_layout));
  const size_t y_size = static_cast<size_t>(y_stride) * height;
  const size_t uv_row_bytes = chroma_width_for(width) * 2;
  const size_t uv_rows = chroma_height_for(height);
  if (stride_bytes < width || static_cast<size_t>(stride_bytes) < uv_row_bytes ||
      frame.bytes.size() < y_size + static_cast<size_t>(uv_stride) * uv_rows) {
    return false;
  }
  auto *dst_y = static_cast<uint8_t *>(vaddr);
  auto *dst_uv = dst_y + static_cast<size_t>(stride_bytes) * height;
  const uint8_t *src_y = frame.bytes.data();
  const uint8_t *src_uv = src_y + y_size;
  copy_plane_rows(dst_y, static_cast<size_t>(stride_bytes), src_y, y_stride,
                  width, height);
  copy_plane_rows(dst_uv, static_cast<size_t>(stride_bytes), src_uv, uv_stride,
                  static_cast<int>(uv_row_bytes), static_cast<int>(uv_rows));
  return true;
}

bool write_ready_nv_to_two_planes(const awesomecam::ReadyI420Frame &frame,
                                  void *dst_y_ptr, int dst_y_stride,
                                  void *dst_uv_ptr, int dst_uv_stride,
                                  bool nv21) {
  const auto wanted_layout =
      nv21 ? awesomecam::kReadyFrameLayoutNV21 : awesomecam::kReadyFrameLayoutNV12;
  if (frame.layout != wanted_layout || dst_y_ptr == nullptr || dst_uv_ptr == nullptr ||
      frame.width <= 0 || frame.height <= 0) {
    return false;
  }
  const int width = frame.width;
  const int height = frame.height;
  const int y_stride = frame.y_stride > 0 ? frame.y_stride : width;
  const int uv_stride = frame.c_stride > 0
                            ? frame.c_stride
                            : static_cast<int>(ready_layout_chroma_row_bytes(width, wanted_layout));
  const size_t y_size = static_cast<size_t>(y_stride) * height;
  const size_t uv_row_bytes = chroma_width_for(width) * 2;
  const size_t uv_rows = chroma_height_for(height);
  if (dst_y_stride < width || static_cast<size_t>(dst_uv_stride) < uv_row_bytes ||
      frame.bytes.size() < y_size + static_cast<size_t>(uv_stride) * uv_rows) {
    return false;
  }
  const uint8_t *src_y = frame.bytes.data();
  const uint8_t *src_uv = src_y + y_size;
  copy_plane_rows(static_cast<uint8_t *>(dst_y_ptr), static_cast<size_t>(dst_y_stride),
                  src_y, y_stride, width, height);
  copy_plane_rows(static_cast<uint8_t *>(dst_uv_ptr), static_cast<size_t>(dst_uv_stride),
                  src_uv, uv_stride, static_cast<int>(uv_row_bytes),
                  static_cast<int>(uv_rows));
  return true;
}

bool try_write_prebuilt_ycbcr_or_i420(const uint8_t *scaled_i420,
                                      size_t scaled_i420_size, int width, int height,
                                      const android_ycbcr &layout) {
  const auto wanted_layout = ready_layout_for_ycbcr(layout);
  awesomecam::RegisterReadyFrameOutputLayout(width, height, wanted_layout);
  if (wanted_layout != awesomecam::kReadyFrameLayoutI420) {
    auto prebuilt = awesomecam::GetReadyFrameForLayout(width, height, wanted_layout);
    if (prebuilt && write_ready_nv_to_ycbcr(*prebuilt, layout)) return true;
  }
  return write_i420_to_ycbcr(scaled_i420, scaled_i420_size, width, height, layout);
}

bool try_write_prebuilt_raw_or_i420(const uint8_t *scaled_i420,
                                    size_t scaled_i420_size, int width, int height,
                                    void *vaddr, int stride_bytes, bool nv21) {
  const auto wanted_layout =
      nv21 ? awesomecam::kReadyFrameLayoutNV21 : awesomecam::kReadyFrameLayoutNV12;
  awesomecam::RegisterReadyFrameOutputLayout(width, height, wanted_layout);
  auto prebuilt = awesomecam::GetReadyFrameForLayout(width, height, wanted_layout);
  if (prebuilt && write_ready_nv_to_contiguous_nv(*prebuilt, vaddr, stride_bytes, nv21)) {
    return true;
  }
  return write_i420_to_contiguous_nv(scaled_i420, scaled_i420_size, width, height,
                                     vaddr, stride_bytes, nv21);
}

bool try_write_prebuilt_two_plane_or_i420(const uint8_t *scaled_i420,
                                          size_t scaled_i420_size, int width,
                                          int height, void *dst_y, int dst_y_stride,
                                          void *dst_uv, int dst_uv_stride,
                                          bool nv21) {
  const auto wanted_layout =
      nv21 ? awesomecam::kReadyFrameLayoutNV21 : awesomecam::kReadyFrameLayoutNV12;
  awesomecam::RegisterReadyFrameOutputLayout(width, height, wanted_layout);
  auto prebuilt = awesomecam::GetReadyFrameForLayout(width, height, wanted_layout);
  if (prebuilt && write_ready_nv_to_two_planes(*prebuilt, dst_y, dst_y_stride,
                                               dst_uv, dst_uv_stride, nv21)) {
    return true;
  }
  if (scaled_i420 == nullptr || scaled_i420_size < awesomecam::I420FrameSize(width, height)) {
    return false;
  }
  const size_t y_size = static_cast<size_t>(width) * height;
  const int chroma_width = static_cast<int>(chroma_width_for(width));
  const uint8_t *src_y = scaled_i420;
  const uint8_t *src_u = src_y + y_size;
  const uint8_t *src_v = src_u + static_cast<size_t>(chroma_width) *
                                   static_cast<size_t>(chroma_height_for(height));
  return nv21
             ? awesomecam::LibYuvI420ToNV21(src_y, width, src_u, chroma_width,
                                            src_v, chroma_width,
                                            static_cast<uint8_t *>(dst_y), dst_y_stride,
                                            static_cast<uint8_t *>(dst_uv), dst_uv_stride,
                                            width, height)
             : awesomecam::LibYuvI420ToNV12(src_y, width, src_u, chroma_width,
                                            src_v, chroma_width,
                                            static_cast<uint8_t *>(dst_y), dst_y_stride,
                                            static_cast<uint8_t *>(dst_uv), dst_uv_stride,
                                            width, height);
}

bool try_write_i420_to_ahardwarebuffer_planes(void *graphic_buffer,
                                              const uint8_t *scaled_i420,
                                              size_t scaled_i420_size,
                                              int width, int height,
                                              const char *where) {
  if (graphic_buffer == nullptr || width <= 0 || height <= 0) return false;
  if (g_graphic_buffer_api.ahb_from_graphic_buffer == nullptr ||
      g_graphic_buffer_api.ahb_lock_planes == nullptr ||
      g_graphic_buffer_api.ahb_unlock == nullptr) {
    return false;
  }

  void *ahb = g_graphic_buffer_api.ahb_from_graphic_buffer(graphic_buffer);
  if (ahb == nullptr) return false;

  AHardwareBufferPlanes planes{};
  AHardwareBufferRect rect{0, 0, width, height};
  const uint64_t lock_start_ns = monotonic_time_ns();
  const int lock_rc = g_graphic_buffer_api.ahb_lock_planes(
      ahb, static_cast<uint64_t>(kGraphicBufferCpuLockUsage), -1, &rect, &planes);
  const uint64_t lock_done_ns = monotonic_time_ns();
  if (lock_rc != 0 || planes.planeCount == 0) {
    const uint64_t fail =
        g_ahb_lock_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fail <= 20 || (fail % 120) == 0) {
      LOGW("%s AHardwareBuffer_lockPlanes failed #%llu rc=%d planes=%u %dx%d",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(fail), lock_rc, planes.planeCount,
           width, height);
    }
    return false;
  }

  bool replaced = false;
  if (planes.planeCount >= 3 && planes.planes[0].data != nullptr &&
      planes.planes[1].data != nullptr && planes.planes[2].data != nullptr) {
    android_ycbcr layout{};
    layout.y = planes.planes[0].data;
    layout.cb = planes.planes[1].data;
    layout.cr = planes.planes[2].data;
    layout.ystride = planes.planes[0].rowStride;
    layout.cstride = planes.planes[1].rowStride;
    layout.chroma_step = planes.planes[1].pixelStride;
    replaced = try_write_prebuilt_ycbcr_or_i420(scaled_i420, scaled_i420_size,
                                                width, height, layout);
  } else if (planes.planeCount >= 2 && planes.planes[0].data != nullptr &&
             planes.planes[1].data != nullptr) {
    const bool nv21 = prefer_raw_nv21_output();
    replaced = try_write_prebuilt_two_plane_or_i420(
        scaled_i420, scaled_i420_size, width, height, planes.planes[0].data,
        static_cast<int>(planes.planes[0].rowStride), planes.planes[1].data,
        static_cast<int>(planes.planes[1].rowStride), nv21);
  }

  int32_t unlock_fence = -1;
  const int unlock_rc = g_graphic_buffer_api.ahb_unlock(ahb, &unlock_fence);
  if (unlock_fence >= 0) close(unlock_fence);
  if (unlock_rc != 0) {
    LOGW("%s AHardwareBuffer_unlock failed rc=%d", where != nullptr ? where : "replace",
         unlock_rc);
  }

  if (replaced) {
    const uint64_t success =
        g_ahb_lock_success_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (success <= 20 || (success % 120) == 0) {
      LOGI("%s AHardwareBuffer_lockPlanes write #%llu planes=%u yStride=%u cStride=%u cStep=%u %dx%d lock=%.3fms",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(success), planes.planeCount,
           planes.planes[0].rowStride,
           planes.planeCount > 1 ? planes.planes[1].rowStride : 0,
           planes.planeCount > 1 ? planes.planes[1].pixelStride : 0,
           width, height, ns_to_ms(lock_done_ns - lock_start_ns));
    }
  }
  return replaced;
}

bool try_write_i420_to_ycbcr_lock(void *graphic_buffer, const uint8_t *frame_bytes,
                                  size_t frame_size, int width, int height,
                                  const char *where, CachedWriteResult *stats) {
  if (graphic_buffer == nullptr || frame_bytes == nullptr) return false;
  android_ycbcr layout{};
  const int lock_rc = g_graphic_buffer_api.lock_ycbcr(graphic_buffer,
                                                      kGraphicBufferCpuLockUsage,
                                                      &layout);
  if (lock_rc != 0) {
    const uint64_t fail_count =
        g_lock_ycbcr_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fail_count <= 20 || (fail_count % 120) == 0) {
      LOGE("%s GraphicBuffer::lockYCbCr failed #%llu rc=%d for %dx%d",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(fail_count), lock_rc, width, height);
    }
    return false;
  }

  const bool replaced = try_write_prebuilt_ycbcr_or_i420(frame_bytes, frame_size,
                                                         width, height, layout);
  const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
  if (unlock_rc != 0) {
    LOGW("%s GraphicBuffer::unlock failed rc=%d",
         where != nullptr ? where : "replace", unlock_rc);
  }
  if (replaced && stats != nullptr) {
    stats->path = BufferLockPath::kYCbCr;
    stats->layout = layout;
  }
  return replaced;
}

bool try_write_i420_to_raw_lock(void *graphic_buffer, const uint8_t *frame_bytes,
                                size_t frame_size, int width, int height,
                                const char *where, CachedWriteResult *stats) {
  if (graphic_buffer == nullptr || frame_bytes == nullptr) return false;
  void *raw_vaddr = nullptr;
  int raw_bpp = 0;
  int raw_stride = 0;
  const uint64_t raw_lock_start_ns = monotonic_time_ns();
  const int raw_rc = g_graphic_buffer_api.lock(graphic_buffer,
                                               kGraphicBufferCpuLockUsage,
                                               &raw_vaddr, &raw_bpp, &raw_stride);
  const uint64_t raw_lock_done_ns = monotonic_time_ns();
  if (raw_rc != 0 || raw_vaddr == nullptr) {
    const uint64_t raw_fail =
        g_raw_lock_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (raw_fail <= 20 || (raw_fail % 120) == 0) {
      LOGE("%s GraphicBuffer::raw lock failed #%llu rc=%d %dx%d bpp=%d stride=%d vaddr=%p",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(raw_fail), raw_rc, width, height,
           raw_bpp, raw_stride, raw_vaddr);
    }
    return false;
  }

  const bool nv21 = prefer_raw_nv21_output();
  const bool replaced = try_write_prebuilt_raw_or_i420(frame_bytes, frame_size, width,
                                                       height, raw_vaddr,
                                                       raw_stride, nv21);
  const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
  if (unlock_rc != 0) {
    LOGW("%s GraphicBuffer::raw unlock failed rc=%d",
         where != nullptr ? where : "replace", unlock_rc);
  }
  const uint64_t raw_success =
      g_raw_lock_success_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (raw_success <= 20 || (raw_success % 120) == 0 || replaced) {
    LOGI("%s GraphicBuffer::raw lock #%llu %dx%d bpp=%d stride=%d nv21=%d replaced=%d lock=%.3fms",
         where != nullptr ? where : "replace",
         static_cast<unsigned long long>(raw_success), width, height, raw_bpp,
         raw_stride, nv21 ? 1 : 0, replaced ? 1 : 0,
         ns_to_ms(raw_lock_done_ns - raw_lock_start_ns));
  }
  if (replaced && stats != nullptr) {
    stats->path = BufferLockPath::kRaw;
    stats->raw_bpp = raw_bpp;
    stats->raw_stride = raw_stride;
    stats->raw_nv21 = nv21;
  }
  return replaced;
}

bool try_write_i420_to_graphic_buffer_cached(void *graphic_buffer, uintptr_t anw_buffer,
                                             int width, int height, int format,
                                             const uint8_t *frame_bytes,
                                             size_t frame_size, const char *where,
                                             CachedWriteResult *stats) {
  if (graphic_buffer == nullptr || frame_bytes == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  BufferLockCacheEntry state =
      lookup_buffer_lock_cache(anw_buffer, width, height, format);
  std::vector<BufferLockPath> order;
  append_unique_lock_path(&order, state.preferred);
  append_unique_lock_path(&order, BufferLockPath::kYCbCr);
  append_unique_lock_path(&order, BufferLockPath::kAhbPlanes);
  append_unique_lock_path(&order, BufferLockPath::kRaw);

  for (BufferLockPath path : order) {
    if (path != state.preferred && buffer_lock_path_failed(state, path)) {
      continue;
    }

    bool replaced = false;
    CachedWriteResult local_stats{};
    switch (path) {
      case BufferLockPath::kYCbCr:
        replaced = try_write_i420_to_ycbcr_lock(graphic_buffer, frame_bytes, frame_size,
                                                width, height, where, &local_stats);
        break;
      case BufferLockPath::kAhbPlanes:
        replaced = try_write_i420_to_ahardwarebuffer_planes(
            graphic_buffer, frame_bytes, frame_size, width, height, where);
        if (replaced) local_stats.path = BufferLockPath::kAhbPlanes;
        break;
      case BufferLockPath::kRaw:
        replaced = try_write_i420_to_raw_lock(graphic_buffer, frame_bytes, frame_size,
                                              width, height, where, &local_stats);
        break;
      case BufferLockPath::kUnknown:
      default:
        break;
    }

    record_buffer_lock_path(anw_buffer, width, height, format, path, replaced, where);
    if (replaced) {
      if (stats != nullptr) *stats = local_stats;
      return true;
    }
    state = lookup_buffer_lock_cache(anw_buffer, width, height, format);
  }

  return false;
}


bool try_replace_camera3_frame(
    const android::camera3::camera_stream_buffer &buffer, int32_t surface_id,
    StreamRecord *matched_stream) {
  if (buffer.stream == nullptr || buffer.buffer == nullptr) return false;
  if (buffer.stream->format != kHalPixelFormatYcbcr420888 &&
      buffer.stream->format != kHalPixelFormatImplementationDefined) {
    return false;
  }
  register_video_target_if_supported(buffer.stream->width, buffer.stream->height,
                                     buffer.stream->format);
  if (!resolve_graphic_buffer_api()) return false;

  const int width = static_cast<int>(buffer.stream->width);
  const int height = static_cast<int>(buffer.stream->height);
  const int format = buffer.stream->format;
  const uint64_t total_start_ns = monotonic_time_ns();
  auto frame = awesomecam::GetReadyI420Frame(width, height);
  if (!frame || frame->bytes.empty()) {
    return false;
  }
  const uint8_t *frame_bytes = frame->bytes.data();
  const size_t frame_size = frame->bytes.size();

  void *anw_buffer = reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(buffer.buffer) - kAnwHandleOffset);
  const uint64_t from_start_ns = monotonic_time_ns();
  GraphicBufferSpRet graphic_buffer_ref = g_graphic_buffer_api.from(anw_buffer);
  void *graphic_buffer = graphic_buffer_ref.ptr;
  const uint64_t from_done_ns = monotonic_time_ns();
  if (graphic_buffer == nullptr) {
    LOGE("GraphicBuffer::from returned null (surfaceId=%d)", surface_id);
    return false;
  }

  const void *dec_strong_cookie = &graphic_buffer_ref;
  CachedWriteResult write_stats{};
  const uint64_t write_start_ns = monotonic_time_ns();
  const bool replaced = try_write_i420_to_graphic_buffer_cached(
      graphic_buffer, reinterpret_cast<uintptr_t>(anw_buffer), width, height, format,
      frame_bytes, frame_size, "returnBufferCheckedLocked", &write_stats);
  const uint64_t write_done_ns = monotonic_time_ns();

  g_graphic_buffer_api.dec_strong(graphic_buffer, dec_strong_cookie);

  if (replaced) {
    const uint64_t replace_count =
        g_replaced_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (replace_count <= 10 || (replace_count % 120) == 0) {
      const uint64_t total_done_ns = monotonic_time_ns();
      LOGI("Replaced frame #%llu streamId=%d dst=%ux%u fmt=%#x source=MediaCodecPlayback gen=%llu pts=%lld surfaceId=%d",
           static_cast<unsigned long long>(replace_count),
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height, buffer.stream->format,
           static_cast<unsigned long long>(frame->generation),
           static_cast<long long>(frame->pts_us), surface_id);
      LOGI("Perf replace #%llu streamId=%d %ux%u path=%s step=%zu from=%.3fms write=%.3fms total=%.3fms rawStride=%d rawNv21=%d",
           static_cast<unsigned long long>(replace_count),
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height,
           buffer_lock_path_name(write_stats.path), write_stats.layout.chroma_step,
           ns_to_ms(from_done_ns - from_start_ns),
           ns_to_ms(write_done_ns - write_start_ns),
           ns_to_ms(total_done_ns - total_start_ns), write_stats.raw_stride,
           write_stats.raw_nv21 ? 1 : 0);
    }
  }

  return replaced;
}


bool try_replace_anw_buffer_direct(void *anw_buffer, int width, int height, int format,
                                   const char *where) {
  if (anw_buffer == nullptr || width <= 0 || height <= 0) return false;
  if (format != kHalPixelFormatYcbcr420888 &&
      format != kHalPixelFormatImplementationDefined) {
    return false;
  }
  register_video_target_if_supported(static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height), format);
  if (!resolve_graphic_buffer_api()) return false;

  auto frame = awesomecam::GetReadyI420Frame(width, height);
  if (!frame || frame->bytes.empty()) {
    return false;
  }
  const uint8_t *frame_bytes = frame->bytes.data();
  const size_t frame_size = frame->bytes.size();

  GraphicBufferSpRet graphic_buffer_ref = g_graphic_buffer_api.from(anw_buffer);
  void *graphic_buffer = graphic_buffer_ref.ptr;
  if (graphic_buffer == nullptr) return false;

  CachedWriteResult write_stats{};
  const bool replaced = try_write_i420_to_graphic_buffer_cached(
      graphic_buffer, reinterpret_cast<uintptr_t>(anw_buffer), width, height, format,
      frame_bytes, frame_size, where, &write_stats);

  g_graphic_buffer_api.dec_strong(graphic_buffer, &graphic_buffer_ref);
  if (replaced) {
    const uint64_t c = g_queue_replaced_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 20 || (c % 120) == 0) {
      LOGI("%s replaced queue frame #%llu dst=%dx%d fmt=%#x path=%s source=MediaCodecPlayback gen=%llu pts=%lld",
           where, static_cast<unsigned long long>(c), width, height, format,
           buffer_lock_path_name(write_stats.path),
           static_cast<unsigned long long>(frame->generation),
           static_cast<long long>(frame->pts_us));
    }
  }
  return replaced;
}


using Camera3DeviceCreateStreamFn = int (*)(
    void *thiz, const void *consumers, bool hasDeferredConsumer, uint32_t width,
    uint32_t height, int format, int32_t dataSpace, int rotation, int *id,
    const void *physicalCameraId, const void *sensorPixelModesUsed, void *surfaceIds,
    int streamSetId, bool isShared, bool isMultiResolution, uint64_t consumerUsage,
    int64_t dynamicRangeProfile, int64_t streamUseCase, int timestampBase,
    int32_t colorSpace, bool useReadoutTimestamp);

// Android 16 changed Camera3OutputStream::returnBufferCheckedLocked from the
// older single surface_id form to:
//   (..., bool output, vector<int> const& surface_ids,
//    vector<size_t> const& transform, sp<Fence>* release_fence)
//
// Keep x5/x6 ABI-stable as opaque pointers.  This also still forwards the old
// int surface_id ABI correctly because the raw x5 register is preserved when we
// tail-call the original.
using ReturnBufferCheckedLockedFn = int (*) (
    void *thiz, const android::camera3::camera_stream_buffer &buffer, long timestamp,
    long readout_timestamp, bool output, const void *surface_ids_or_legacy_id,
    const void *transform, android::sp<android::Fence> *release_fence);


using QueueBufferToConsumerFn = int (*)(void *thiz, void *consumer_sp,
                                        void *anw_buffer, int anw_release_fence,
                                        const void *surface_ids);

int hook_queue_buffer_to_consumer(void *thiz, void *consumer_sp, void *anw_buffer,
                                  int anw_release_fence, const void *surface_ids) {
  SHADOWHOOK_STACK_SCOPE();
  auto orig = reinterpret_cast<QueueBufferToConsumerFn>(g_orig_queue_buffer_to_consumer);
  if (orig == nullptr) return -1;

  const uint64_t count =
      g_queue_buffer_to_consumer_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (anw_buffer != nullptr) {
    const auto *prefix = reinterpret_cast<const ANativeWindowBufferPrefix *>(anw_buffer);
    const int width = prefix->width;
    const int height = prefix->height;
    const int format = prefix->format;
    if (count <= 20 || (count % 120) == 0) {
      LOGI("queueBufferToConsumer hit #%llu anw=%p %dx%d stride=%d fmt=%#x usage=%#" PRIx64,
           static_cast<unsigned long long>(count), anw_buffer, width, height,
           prefix->stride, format, prefix->usage);
    }
    if (width > 0 && height > 0) {
      register_video_target_if_supported(static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height), format);
    }
    if (write_mode_allows_camera3(current_write_mode())) {
      (void)wait_fence_if_valid(anw_release_fence,
                                "queueBufferToConsumer release",
                                kDefaultFenceWaitTimeoutMs);
      (void)try_replace_anw_buffer_direct(anw_buffer, width, height, format,
                                          "queueBufferToConsumer");
    }
  } else if (count <= 20) {
    LOGI("queueBufferToConsumer hit #%llu anw=null", static_cast<unsigned long long>(count));
  }

  return orig(thiz, consumer_sp, anw_buffer, anw_release_fence, surface_ids);
}


using SurfaceHookQueueBufferFn = int (*)(void *window, void *anw_buffer, int fence_fd);
using SurfaceSetUsageFn = int (*)(void *thiz, uint64_t usage);

int hook_surface_set_usage(void *thiz, uint64_t usage) {
  SHADOWHOOK_STACK_SCOPE();
  auto orig = reinterpret_cast<SurfaceSetUsageFn>(g_orig_surface_set_usage);
  if (orig == nullptr) return -1;

  const uint64_t forced_usage = usage | kGrallocUsageCpuReadWriteOften;
  const uint64_t count =
      g_surface_set_usage_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 30 || usage != forced_usage || (count % 120) == 0) {
    LOGI("Surface::setUsage hit #%llu this=%p usage=%#" PRIx64 "->%#" PRIx64,
         static_cast<unsigned long long>(count), thiz, usage, forced_usage);
  }
  return orig(thiz, forced_usage);
}

int hook_surface_hook_queue_buffer(void *window, void *anw_buffer, int fence_fd) {
  SHADOWHOOK_STACK_SCOPE();
  auto orig = reinterpret_cast<SurfaceHookQueueBufferFn>(g_orig_surface_hook_queue_buffer);
  if (orig == nullptr) return -1;

  const uint64_t count =
      g_surface_queue_buffer_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (anw_buffer != nullptr) {
    const auto *prefix = reinterpret_cast<const ANativeWindowBufferPrefix *>(anw_buffer);
    const int width = prefix->width;
    const int height = prefix->height;
    const int format = prefix->format;
    if (count <= 20 || (count % 120) == 0) {
      LOGI("Surface::hook_queueBuffer hit #%llu window=%p anw=%p %dx%d stride=%d fmt=%#x usage=%#" PRIx64,
           static_cast<unsigned long long>(count), window, anw_buffer, width, height,
           prefix->stride, format, prefix->usage);
    }
    if (write_mode_allows_surface(current_write_mode())) {
      const bool fence_ready =
          wait_fence_if_valid(fence_fd, "Surface::hook_queueBuffer fence",
                              kSurfaceFenceWaitTimeoutMs);
      if (fence_ready) {
        (void)try_replace_anw_buffer_direct(anw_buffer, width, height, format,
                                            "Surface::hook_queueBuffer");
      }
    }
  } else if (count <= 20) {
    LOGI("Surface::hook_queueBuffer hit #%llu anw=null", static_cast<unsigned long long>(count));
  }
  return orig(window, anw_buffer, fence_fd);
}

int hook_camera3device_create_stream(
    void *thiz, const void *consumers, bool hasDeferredConsumer, uint32_t width,
    uint32_t height, int format, int32_t dataSpace, int rotation, int *id,
    const void *physicalCameraId, const void *sensorPixelModesUsed, void *surfaceIds,
    int streamSetId, bool isShared, bool isMultiResolution, uint64_t consumerUsage,
    int64_t dynamicRangeProfile, int64_t streamUseCase, int timestampBase,
    int32_t colorSpace, bool useReadoutTimestamp) {
  SHADOWHOOK_STACK_SCOPE();

  auto orig = reinterpret_cast<Camera3DeviceCreateStreamFn>(g_orig_camera3device_create_stream);
  if (orig == nullptr) {
    LOGE("Camera3Device::createStream orig is NULL");
    return -1;
  }

  const int requested_format = format;
  const uint64_t requested_consumer_usage = consumerUsage;
  int actual_format = format;
  uint64_t actual_consumer_usage = consumerUsage;
  const bool forced_yuv =
      should_force_implementation_defined_preview_to_yuv(width, height, format,
                                                         dataSpace, consumerUsage);
  if (forced_yuv) {
    actual_format = kHalPixelFormatYcbcr420888;
    actual_consumer_usage |= kGrallocUsageCpuReadWriteOften;
    LOGI("Camera3Device::createStream force preview stream format %#x -> %#x width=%u height=%u dataspace=%#x usage=%" PRIu64 "->%" PRIu64,
         requested_format, actual_format, width, height, dataSpace,
         requested_consumer_usage, actual_consumer_usage);
  }

  const int res = orig(thiz, consumers, hasDeferredConsumer, width, height, actual_format,
                       dataSpace, rotation, id, physicalCameraId, sensorPixelModesUsed,
                       surfaceIds, streamSetId, isShared, isMultiResolution,
                       actual_consumer_usage, dynamicRangeProfile, streamUseCase, timestampBase,
                       colorSpace, useReadoutTimestamp);

  const uint64_t count =
      g_create_stream_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const int streamId = (id != nullptr) ? *id : -1;
  if (res == 0) {
    remember_stream_record(streamId, width, height, actual_format, dataSpace, actual_consumer_usage,
                           streamSetId, hasDeferredConsumer, isShared,
                           isMultiResolution, timestampBase, colorSpace,
                           useReadoutTimestamp);
  }

  LOGI("Camera3Device::createStream hit #%llu res=%d streamId=%d width=%u height=%u format=%#x requested=%#x forcedYuv=%d dataspace=%#x deferred=%d shared=%d multiRes=%d usage=%" PRIu64 " streamSetId=%d tsBase=%d colorSpace=%d readoutTs=%d",
       static_cast<unsigned long long>(count), res, streamId, width, height, actual_format,
       requested_format, forced_yuv ? 1 : 0, dataSpace, hasDeferredConsumer,
       isShared, isMultiResolution, actual_consumer_usage,
       streamSetId, timestampBase, colorSpace, useReadoutTimestamp);

  return res;
}

int hook_return_buffer_checked_locked(
    void *thiz, const android::camera3::camera_stream_buffer &buffer, long timestamp,
    long readout_timestamp, bool output, const void *surface_ids_or_legacy_id,
    const void *transform, android::sp<android::Fence> *release_fence) {
  SHADOWHOOK_STACK_SCOPE();

  auto orig = reinterpret_cast<ReturnBufferCheckedLockedFn>(g_orig_return_buffer_checked_locked);
  if (orig == nullptr) {
    LOGE("returnBufferCheckedLocked orig is NULL");
    return -1;
  }

  const uint64_t count =
      g_return_buffer_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;

  StreamRecord matched{};
  bool found_match = false;
  uint64_t per_stream_log_count = 0;
  const uintptr_t raw_surface_arg =
      reinterpret_cast<uintptr_t>(surface_ids_or_legacy_id);
  const int32_t surface_id_for_log =
      raw_surface_arg <= 0xffffffffULL ? static_cast<int32_t>(raw_surface_arg) : -1;

  const auto *stream = buffer.stream;
  if (stream != nullptr) {
    // The createStream symbol may be absent on stripped cameraserver builds.
    // Learn target sizes from actual returned buffers as a symbol-only fallback.
    register_video_target_if_supported(stream->width, stream->height, stream->format);

    found_match = find_stream_record_by_props(stream->width, stream->height,
                                              stream->format, stream->data_space,
                                              &matched, &per_stream_log_count);
    if (found_match) {
      if (per_stream_log_count <= 5) {
        LOGI("returnBufferCheckedLocked hit #%llu streamId=%d width=%u height=%u format=%#x dataspace=%#x usage=%#" PRIx64 " output=%d surfaceArg=%p surfaceId=%d ts=%ld readoutTs=%ld",
             static_cast<unsigned long long>(count), matched.stream_id, stream->width,
             stream->height, stream->format, stream->data_space, stream->usage,
             output, surface_ids_or_legacy_id, surface_id_for_log, timestamp,
             readout_timestamp);
      }
    } else if (count <= 12) {
      LOGI("returnBufferCheckedLocked hit #%llu unmatched width=%u height=%u format=%#x dataspace=%#x usage=%#" PRIx64 " output=%d surfaceArg=%p surfaceId=%d ts=%ld readoutTs=%ld",
           static_cast<unsigned long long>(count), stream->width, stream->height,
           stream->format, stream->data_space, stream->usage, output,
           surface_ids_or_legacy_id, surface_id_for_log, timestamp,
           readout_timestamp);
    }
  } else if (count <= 12) {
    LOGI("returnBufferCheckedLocked hit #%llu stream=null output=%d surfaceArg=%p surfaceId=%d ts=%ld readoutTs=%ld",
         static_cast<unsigned long long>(count), output, surface_ids_or_legacy_id,
         surface_id_for_log, timestamp, readout_timestamp);
  }

  if (output && stream != nullptr &&
      write_mode_allows_camera3(current_write_mode())) {
    // Camera HAL may return a buffer with an unsignaled acquire fence.  If we
    // CPU-write before that fence signals, HAL/GPU can still overwrite our
    // replacement and the app sees the original frame.  Wait first, then write.
    (void)wait_fence_if_valid(buffer.acquire_fence,
                              "returnBufferCheckedLocked acquire",
                              kDefaultFenceWaitTimeoutMs);
    (void)try_replace_camera3_frame(buffer, surface_id_for_log,
                                    found_match ? &matched : nullptr);
  }

  return orig(thiz, buffer, timestamp, readout_timestamp, output,
              surface_ids_or_legacy_id, transform, release_fence);
}

void install_hook() {
  LOGI("install_hook: begin");

  const int init_rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
  if (init_rc != 0) {
    const int err = shadowhook_get_errno();
    LOGE("shadowhook_init failed rc=%d errno=%d msg=%s", init_rc, err,
         shadowhook_to_errmsg(err));
    return;
  }
  LOGI("install_hook: shadowhook initialized");

  if (!resolve_graphic_buffer_api()) {
    LOGE("install_hook: GraphicBuffer API unresolved");
    return;
  }

  const WriteMode initial_write_mode = current_write_mode();
  LOGI("install_hook: write mode=%s (default surfaceOnly; touch %s for all hooks, %s for Camera3-only)",
       write_mode_name(initial_write_mode), kWriteAllFlagPath, kWriteCamera3FlagPath);

  g_surface_set_usage_stub = shadowhook_hook_sym_name(
      "libgui.so",
      "_ZN7android7Surface8setUsageEm",
      reinterpret_cast<void *>(hook_surface_set_usage),
      &g_orig_surface_set_usage);
  if (g_surface_set_usage_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGE("hook Surface::setUsage failed errno=%d msg=%s", err,
         shadowhook_to_errmsg(err));
  } else {
    LOGI("installed hook for android::Surface::setUsage orig=%p",
         g_orig_surface_set_usage);
  }

  g_surface_hook_queue_buffer_stub = shadowhook_hook_sym_name(
      "libgui.so",
      "_ZN7android7Surface16hook_queueBufferEP13ANativeWindowP19ANativeWindowBufferi",
      reinterpret_cast<void *>(hook_surface_hook_queue_buffer),
      &g_orig_surface_hook_queue_buffer);
  if (g_surface_hook_queue_buffer_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGE("hook Surface::hook_queueBuffer failed errno=%d msg=%s", err,
         shadowhook_to_errmsg(err));
  } else {
    LOGI("installed hook for android::Surface::hook_queueBuffer orig=%p",
         g_orig_surface_hook_queue_buffer);
  }

  LOGI("install_hook: offset-based cameraserver hooks disabled; installing cameraserver hooks by symbol only");

  g_camera3device_create_stream_stub = shadowhook_hook_sym_name(
      kTargetModuleBasename,
      kCamera3DeviceCreateStreamMangledSymbol,
      reinterpret_cast<void *>(hook_camera3device_create_stream),
      &g_orig_camera3device_create_stream);
  if (g_camera3device_create_stream_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGW("symbol hook %s failed errno=%d msg=%s",
         kCamera3DeviceCreateStreamSymbol, err, shadowhook_to_errmsg(err));
  } else {
    LOGI("installed symbol hook for %s orig=%p",
         kCamera3DeviceCreateStreamSymbol, g_orig_camera3device_create_stream);
  }

  g_queue_buffer_to_consumer_stub = shadowhook_hook_sym_name(
      kTargetModuleBasename,
      kQueueBufferToConsumerMangledSymbol,
      reinterpret_cast<void *>(hook_queue_buffer_to_consumer),
      &g_orig_queue_buffer_to_consumer);
  if (g_queue_buffer_to_consumer_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGW("symbol hook %s failed errno=%d msg=%s",
         kQueueBufferToConsumerSymbol, err, shadowhook_to_errmsg(err));
  } else {
    LOGI("installed symbol hook for %s orig=%p",
         kQueueBufferToConsumerSymbol, g_orig_queue_buffer_to_consumer);
  }

  g_return_buffer_checked_locked_stub = shadowhook_hook_sym_name(
      kTargetModuleBasename,
      kReturnBufferCheckedLockedMangledSymbol,
      reinterpret_cast<void *>(hook_return_buffer_checked_locked),
      &g_orig_return_buffer_checked_locked);
  if (g_return_buffer_checked_locked_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGW("symbol hook %s failed errno=%d msg=%s",
         kReturnBufferCheckedLockedSymbol, err, shadowhook_to_errmsg(err));
  } else {
    LOGI("installed symbol hook for %s orig=%p",
         kReturnBufferCheckedLockedSymbol, g_orig_return_buffer_checked_locked);
  }
}

}  // namespace

namespace {

void EnsureSelfPinned(const void *symbol_for_dladdr, const char *entry_name) {
  if (g_self_handle != nullptr) return;
  Dl_info self_info{};
  if (dladdr(symbol_for_dladdr, &self_info) != 0 && self_info.dli_fname != nullptr) {
    int flags = RTLD_NOW | RTLD_GLOBAL;
#ifdef RTLD_NODELETE
    flags |= RTLD_NODELETE;
#endif
    g_self_handle = dlopen(self_info.dli_fname, flags);
    if (g_self_handle != nullptr) {
      LOGI("%s: self-pinned %s handle=%p", entry_name, self_info.dli_fname, g_self_handle);
    } else {
      LOGE("%s: self-pin failed for %s: %s", entry_name, self_info.dli_fname,
           dlerror() != nullptr ? dlerror() : "unknown");
    }
  } else {
    LOGE("%s: dladdr failed for self", entry_name);
  }
}
}

extern "C" __attribute__((visibility("default"))) void main_hook_c61() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c61), "main_hook_c61");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C61]: already ran");
    return;
  }
  LOGI("main_hook[C61]: classic threadpool worker only");
  const bool ok = awesomecam::ProbeClassicThreadPoolWorkerOnly();
  LOGI("main_hook[C61]: ProbeClassicThreadPoolWorkerOnly => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c62() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c62), "main_hook_c62");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C62]: already ran");
    return;
  }
  LOGI("main_hook[C62]: classic service worker async");
  const bool ok = awesomecam::ProbeClassicServiceWorkerAsync();
  LOGI("main_hook[C62]: ProbeClassicServiceWorkerAsync => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook), "main_hook");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook: already ran");
    return;
  }
  LOGI("main_hook: install hooks + classic service worker async");
  install_hook();
  const bool ok = awesomecam::ProbeClassicServiceWorkerAsync();
  LOGI("main_hook: ProbeClassicServiceWorkerAsync => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c51() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c51), "main_hook_c51");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C51]: already ran");
    return;
  }
  LOGI("main_hook[C51]: threadpool worker only");
  const bool ok = awesomecam::ProbeBinderThreadPoolWorkerOnly();
  LOGI("main_hook[C51]: ProbeBinderThreadPoolWorkerOnly => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c52() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c52), "main_hook_c52");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C52]: already ran");
    return;
  }
  LOGI("main_hook[C52]: full service worker async");
  const bool ok = awesomecam::ProbeBinderFullServiceWorkerAsync();
  LOGI("main_hook[C52]: ProbeBinderFullServiceWorkerAsync => %s", ok ? "ok" : "fail");
}
