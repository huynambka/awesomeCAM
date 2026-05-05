#include <android/log.h>
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "libyuv_runtime.h"
#include "ready_frame_cache.h"
#include "shadowhook.h"
#include "video2camera_service.h"
#include "video2camera_player.h"
#include "video2camera_ipc.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void *g_orig_create_configured_surface = nullptr;
extern "C" void *g_orig_camera3device_create_stream = nullptr;
extern "C" void *g_orig_return_buffer_checked_locked = nullptr;
extern "C" void *g_orig_queue_buffer_to_consumer = nullptr;
extern "C" void *g_orig_surface_hook_queue_buffer = nullptr;
extern "C" void *g_orig_surface_set_usage = nullptr;

extern "C" void log_create_configured_surface_result(void *stream_info_ptr,
                                                       uint64_t is_stream_info_valid);
extern "C" __attribute__((naked, noinline)) void hook_create_configured_surface();

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
constexpr const char *kOffsetConfigPath = "/data/camera/awesomecam_offsets.conf";
constexpr const char *kKnownCameraServerBuildId =
    "ce32331d43edd54bcfd0b07af58823fe";
// Offsets for Pixel 6a Android 16 cameraserver BuildId
// ce32331d43edd54bcfd0b07af58823fe.
//
// The previous Android-15 offsets were:
//   createConfiguredSurface:       0x2b8e00
//   Camera3Device::createStream:   0x1f05c0
//   returnBufferCheckedLocked:     0x22cd70
// On this build, 0x1f05c0 is InFlightRequest::~InFlightRequest(), so it killed
// cameraserver in RequestThread. Keep these as one clearly-owned config block.
constexpr uintptr_t kCreateConfiguredSurfaceOffset = 0x2c0fd0;
constexpr uintptr_t kCamera3DeviceCreateStreamOffset = 0x1f5370;
constexpr uintptr_t kReturnBufferCheckedLockedOffset = 0x234030;
constexpr uintptr_t kQueueBufferToConsumerOffset = 0x23c680;

// createConfiguredSurface has many stack args and changes often; createStream
// and returnBuffer are enough for preview replacement + ReadyFrameCache targets.
constexpr bool kEnableCreateConfiguredSurfaceHook = false;
constexpr bool kEnableCamera3DeviceCreateStreamHook = true;
constexpr bool kEnableReturnBufferCheckedLockedHook = true;
constexpr bool kEnableQueueBufferToConsumerHook = true;

constexpr int kHalPixelFormatBlob = 0x21;
constexpr int kHalPixelFormatImplementationDefined = 0x22;
constexpr int kHalPixelFormatYcbcr420888 = 0x23;
constexpr int kHalPixelFormatRaw10 = 0x25;
constexpr int32_t kDataspaceJfif = 0x8c20000;
constexpr bool kForceImplementationDefinedPreviewToYuv = true;

constexpr const char *kCreateConfiguredSurfaceSymbol =
    "android::camera3::SessionConfigurationUtils::createConfiguredSurface "
    "(createSurfaceFromGbp equivalent)";
constexpr const char *kCamera3DeviceCreateStreamSymbol =
    "android::Camera3Device::createStream(std::vector<SurfaceHolder> const&, ...)";
constexpr const char *kReturnBufferCheckedLockedSymbol =
    "android::camera3::Camera3OutputStream::returnBufferCheckedLocked";
constexpr const char *kQueueBufferToConsumerSymbol =
    "android::camera3::Camera3OutputStream::queueBufferToConsumer";

constexpr const char *kSourceMetaPath = "/data/camera/source.meta";
constexpr const char *kSourceFramesDir = "/data/camera/frames";
constexpr const char *kSingleFrameCandidates[] = {
    "/data/camera/frame.i420",   "/data/camera/frame.nv12",
    "/data/camera/frame.nv21",   "/data/camera/input.i420",
    "/data/camera/input.nv12",   "/data/camera/input.nv21",
    "/data/camera/replace.i420", "/data/camera/replace.nv12",
    "/data/camera/replace.nv21", "/data/camera/video.i420",
    "/data/camera/video.nv12",   "/data/camera/video.nv21",
};

constexpr uint32_t kGraphicBufferCpuLockUsage = 0x33;
constexpr uint64_t kGrallocUsageCpuReadWriteOften = 0x33;
constexpr uintptr_t kAnwHandleOffset = 0x60;
constexpr uint64_t kSourceRescanIntervalNs = 1000000000ULL;
constexpr int kDefaultSourceFps = 30;
constexpr uint64_t kNsPerSecond = 1000000000ULL;

enum class SourcePixelFormat {
  kUnknown = 0,
  kI420,
  kNV12,
  kNV21,
};

struct OutputStreamInfoPrefix {
  int32_t width;
  int32_t height;
  int32_t format;
  int32_t data_space;
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

struct SourceDescriptor {
  bool valid = false;
  bool sequence = false;
  std::string path;
  std::string dir;
  int width = 0;
  int height = 0;
  int fps = kDefaultSourceFps;
  SourcePixelFormat format = SourcePixelFormat::kUnknown;
};

struct HookOffsetConfig {
  uintptr_t create_configured_surface = kCreateConfiguredSurfaceOffset;
  uintptr_t create_stream = kCamera3DeviceCreateStreamOffset;
  uintptr_t return_buffer_checked_locked = kReturnBufferCheckedLockedOffset;
  bool enable_create_configured_surface = kEnableCreateConfiguredSurfaceHook;
  bool enable_create_stream = kEnableCamera3DeviceCreateStreamHook;
  bool enable_return_buffer_checked_locked = kEnableReturnBufferCheckedLockedHook;
  std::string expected_build_id = kKnownCameraServerBuildId;
  bool from_file = false;
};

struct ElfRange {
  size_t file_offset = 0;
  uintptr_t vaddr = 0;
  size_t size = 0;
};

struct CameraServerElfView {
  std::vector<uint8_t> bytes;
  ElfRange text;
  ElfRange rodata;
};

struct SourceState {
  std::mutex mutex;
  SourceDescriptor descriptor;
  std::vector<std::string> sequence_paths;
  size_t next_sequence_index = 0;
  std::string loaded_path;
  time_t loaded_mtime = 0;
  off_t loaded_size = 0;
  std::vector<uint8_t> frame_bytes;
  uint64_t frame_interval_ns = kNsPerSecond / kDefaultSourceFps;
  uint64_t next_frame_deadline_ns = 0;
  uint64_t last_scan_ns = 0;
  bool missing_logged = false;
  uint64_t load_generation = 0;
};

enum class ScaledFrameSourceKind {
  kNone = 0,
  kBinder = 1,
  kLocal = 2,
};

struct ScaledFrameCacheEntry {
  int dst_width = 0;
  int dst_height = 0;
  SourceDescriptor source_descriptor;
  std::vector<uint8_t> scaled_i420;
};

struct ScaledFrameCacheState {
  ScaledFrameSourceKind source_kind = ScaledFrameSourceKind::kNone;
  uint64_t source_generation = 0;
  std::vector<ScaledFrameCacheEntry> entries;
};

struct I420View {
  const uint8_t *y = nullptr;
  const uint8_t *u = nullptr;
  const uint8_t *v = nullptr;
  int width = 0;
  int height = 0;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
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
std::atomic<uint64_t> g_surface_hit_count{0};
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

void *g_create_surface_stub = nullptr;
void *g_camera3device_create_stream_stub = nullptr;
void *g_return_buffer_checked_locked_stub = nullptr;
void *g_queue_buffer_to_consumer_stub = nullptr;
void *g_surface_hook_queue_buffer_stub = nullptr;
void *g_surface_set_usage_stub = nullptr;

std::mutex g_stream_records_mutex;
std::vector<StreamRecord> g_stream_records;
std::mutex g_frame_process_mutex;
SourceState g_source_state;
ScaledFrameCacheState g_scaled_frame_cache;
GraphicBufferApi g_graphic_buffer_api;
void *g_self_handle = nullptr;

bool ends_with(const char *value, const char *suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t value_len = strlen(value);
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > value_len) return false;
  return memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

bool ends_with_ci(const std::string &value, const char *suffix) {
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > value.size()) return false;
  for (size_t i = 0; i < suffix_len; ++i) {
    char a = value[value.size() - suffix_len + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::string trim_copy(const std::string &value) {
  size_t begin = 0;
  while (begin < value.size()) {
    const char c = value[begin];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    ++begin;
  }

  size_t end = value.size();
  while (end > begin) {
    const char c = value[end - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    --end;
  }

  return value.substr(begin, end - begin);
}

std::string basename_copy(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

bool parse_int_value(const std::string &text, int *out) {
  if (out == nullptr) return false;
  char *end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<int>(parsed);
  return true;
}

bool parse_dimensions_from_string(const std::string &text, int *width, int *height) {
  if (width == nullptr || height == nullptr) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    int w = 0;
    int h = 0;
    if (sscanf(text.c_str() + i, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      *width = w;
      *height = h;
      return true;
    }
  }
  return false;
}

const char *source_pixel_format_name(SourcePixelFormat format) {
  switch (format) {
    case SourcePixelFormat::kI420:
      return "I420";
    case SourcePixelFormat::kNV12:
      return "NV12";
    case SourcePixelFormat::kNV21:
      return "NV21";
    default:
      return "unknown";
  }
}

SourcePixelFormat infer_source_pixel_format(const std::string &path) {
  if (ends_with_ci(path, ".i420")) return SourcePixelFormat::kI420;
  if (ends_with_ci(path, ".nv12")) return SourcePixelFormat::kNV12;
  if (ends_with_ci(path, ".nv21")) return SourcePixelFormat::kNV21;
  return SourcePixelFormat::kUnknown;
}

bool parse_source_pixel_format(const std::string &text, SourcePixelFormat *out) {
  if (out == nullptr) return false;
  std::string lower = trim_copy(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(::tolower(c));
  });
  if (lower == "i420") {
    *out = SourcePixelFormat::kI420;
    return true;
  }
  if (lower == "nv12") {
    *out = SourcePixelFormat::kNV12;
    return true;
  }
  if (lower == "nv21") {
    *out = SourcePixelFormat::kNV21;
    return true;
  }
  return false;
}

uint64_t monotonic_time_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond +
         static_cast<uint64_t>(ts.tv_nsec);
}

double ns_to_ms(uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
}

bool stat_path(const std::string &path, struct stat *st) {
  if (st == nullptr) return false;
  return stat(path.c_str(), st) == 0;
}

bool is_directory_path(const std::string &path) {
  struct stat st {};
  return stat_path(path, &st) && S_ISDIR(st.st_mode);
}

bool load_file_bytes(const std::string &path, std::vector<uint8_t> *out, time_t *mtime,
                     off_t *size) {
  if (out == nullptr) return false;

  struct stat st {};
  if (!stat_path(path, &st) || !S_ISREG(st.st_mode)) return false;

  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr) return false;

  std::vector<uint8_t> bytes(static_cast<size_t>(st.st_size));
  const size_t read = bytes.empty() ? 0 : fread(bytes.data(), 1, bytes.size(), fp);
  fclose(fp);
  if (read != bytes.size()) return false;

  *out = std::move(bytes);
  if (mtime != nullptr) *mtime = st.st_mtime;
  if (size != nullptr) *size = st.st_size;
  return true;
}

std::vector<std::string> list_supported_frame_files(const std::string &dir) {
  std::vector<std::string> files;
  DIR *dp = opendir(dir.c_str());
  if (dp == nullptr) return files;

  while (struct dirent *entry = readdir(dp)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    std::string full = dir;
    if (!full.empty() && full.back() != '/') full.push_back('/');
    full += entry->d_name;

    struct stat st {};
    if (!stat_path(full, &st) || !S_ISREG(st.st_mode)) continue;
    if (infer_source_pixel_format(full) == SourcePixelFormat::kUnknown) continue;
    files.push_back(full);
  }

  closedir(dp);
  std::sort(files.begin(), files.end());
  return files;
}

bool parse_source_meta(SourceDescriptor *descriptor) {
  if (descriptor == nullptr) return false;

  FILE *fp = fopen(kSourceMetaPath, "re");
  if (fp == nullptr) return false;

  SourceDescriptor meta;
  meta.fps = kDefaultSourceFps;

  char line[512];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    std::string raw = trim_copy(line);
    if (raw.empty() || raw[0] == '#') continue;
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) continue;

    const std::string key = trim_copy(raw.substr(0, eq));
    const std::string value = trim_copy(raw.substr(eq + 1));

    if (key == "path") {
      meta.path = value;
    } else if (key == "dir") {
      meta.dir = value;
    } else if (key == "width") {
      parse_int_value(value, &meta.width);
    } else if (key == "height") {
      parse_int_value(value, &meta.height);
    } else if (key == "fps") {
      parse_int_value(value, &meta.fps);
    } else if (key == "format") {
      parse_source_pixel_format(value, &meta.format);
    }
  }

  fclose(fp);

  meta.sequence = !meta.dir.empty();
  meta.valid = meta.sequence ? !meta.dir.empty() : !meta.path.empty();
  *descriptor = meta;
  return meta.valid;
}

bool infer_dimensions_from_file(const std::string &path, off_t file_size, int hint_width,
                                int hint_height, int *width, int *height) {
  if (width == nullptr || height == nullptr) return false;

  if (parse_dimensions_from_string(basename_copy(path), width, height)) return true;

  if (hint_width > 0 && hint_height > 0) {
    const size_t chroma_width = static_cast<size_t>((hint_width + 1) / 2);
    const size_t chroma_height = static_cast<size_t>((hint_height + 1) / 2);
    const size_t expected = static_cast<size_t>(hint_width) * hint_height +
                            2 * chroma_width * chroma_height;
    if (static_cast<off_t>(expected) == file_size) {
      *width = hint_width;
      *height = hint_height;
      return true;
    }
  }

  return false;
}

SourceDescriptor discover_source_descriptor(int hint_width, int hint_height,
                                            std::vector<std::string> *sequence_paths) {
  SourceDescriptor descriptor;
  if (sequence_paths != nullptr) sequence_paths->clear();

  if (parse_source_meta(&descriptor)) {
    if (descriptor.sequence) {
      if (sequence_paths != nullptr) {
        *sequence_paths = list_supported_frame_files(descriptor.dir);
      }
      descriptor.valid = sequence_paths != nullptr && !sequence_paths->empty();
      if (descriptor.valid && descriptor.format == SourcePixelFormat::kUnknown) {
        descriptor.format = infer_source_pixel_format((*sequence_paths)[0]);
      }
      if (descriptor.valid && (descriptor.width <= 0 || descriptor.height <= 0)) {
        struct stat st {};
        if (stat_path((*sequence_paths)[0], &st)) {
          infer_dimensions_from_file((*sequence_paths)[0], st.st_size, hint_width,
                                     hint_height, &descriptor.width,
                                     &descriptor.height);
        }
      }
      return descriptor;
    }

    descriptor.sequence = false;
    descriptor.valid = !descriptor.path.empty();
    if (descriptor.valid && descriptor.format == SourcePixelFormat::kUnknown) {
      descriptor.format = infer_source_pixel_format(descriptor.path);
    }
    if (descriptor.valid && (descriptor.width <= 0 || descriptor.height <= 0)) {
      struct stat st {};
      if (stat_path(descriptor.path, &st)) {
        infer_dimensions_from_file(descriptor.path, st.st_size, hint_width, hint_height,
                                   &descriptor.width, &descriptor.height);
      }
    }
    return descriptor;
  }

  if (is_directory_path(kSourceFramesDir)) {
    std::vector<std::string> discovered = list_supported_frame_files(kSourceFramesDir);
    if (!discovered.empty()) {
      descriptor.valid = true;
      descriptor.sequence = true;
      descriptor.dir = kSourceFramesDir;
      descriptor.fps = kDefaultSourceFps;
      descriptor.format = infer_source_pixel_format(discovered[0]);
      if (sequence_paths != nullptr) *sequence_paths = discovered;
      struct stat st {};
      if (stat_path(discovered[0], &st)) {
        infer_dimensions_from_file(discovered[0], st.st_size, hint_width, hint_height,
                                   &descriptor.width, &descriptor.height);
      }
      return descriptor;
    }
  }

  for (const char *candidate : kSingleFrameCandidates) {
    struct stat st {};
    if (!stat_path(candidate, &st) || !S_ISREG(st.st_mode)) continue;

    descriptor.valid = true;
    descriptor.sequence = false;
    descriptor.path = candidate;
    descriptor.fps = kDefaultSourceFps;
    descriptor.format = infer_source_pixel_format(candidate);
    infer_dimensions_from_file(candidate, st.st_size, hint_width, hint_height,
                               &descriptor.width, &descriptor.height);
    return descriptor;
  }

  descriptor.valid = false;
  return descriptor;
}

bool ensure_source_frame_locked(int hint_width, int hint_height,
                                SourceDescriptor *active_descriptor) {
  const uint64_t now_ns = monotonic_time_ns();
  const bool should_rescan =
      (g_source_state.last_scan_ns == 0 ||
       now_ns - g_source_state.last_scan_ns >= kSourceRescanIntervalNs ||
       !g_source_state.descriptor.valid);

  if (should_rescan) {
    std::vector<std::string> sequence_paths;
    SourceDescriptor discovered =
        discover_source_descriptor(hint_width, hint_height, &sequence_paths);
    g_source_state.last_scan_ns = now_ns;

    if (!discovered.valid || discovered.format == SourcePixelFormat::kUnknown ||
        discovered.width <= 0 || discovered.height <= 0) {
      g_source_state.descriptor = SourceDescriptor{};
      g_source_state.sequence_paths.clear();
      g_source_state.frame_bytes.clear();
      g_source_state.loaded_path.clear();
      g_source_state.loaded_mtime = 0;
      g_source_state.loaded_size = 0;
      if (!g_source_state.missing_logged) {
        LOGW("No usable source under /data/camera (expected source.meta, frames/*.i420|nv12|nv21, or frame/input/replace/video raw files)");
        g_source_state.missing_logged = true;
      }
      return false;
    }

    g_source_state.descriptor = discovered;
    g_source_state.sequence_paths = std::move(sequence_paths);
    g_source_state.frame_interval_ns =
        kNsPerSecond /
        static_cast<uint64_t>(std::max(1, std::min(240, discovered.fps)));
    g_source_state.missing_logged = false;

    if (!discovered.sequence) {
      g_source_state.next_sequence_index = 0;
    } else if (!g_source_state.sequence_paths.empty()) {
      g_source_state.next_sequence_index %= g_source_state.sequence_paths.size();
    }
  }

  if (!g_source_state.descriptor.valid) return false;

  bool need_reload = false;
  std::string selected_path;

  if (g_source_state.descriptor.sequence) {
    if (g_source_state.sequence_paths.empty()) return false;
    if (g_source_state.loaded_path.empty() || now_ns >= g_source_state.next_frame_deadline_ns) {
      selected_path = g_source_state.sequence_paths[g_source_state.next_sequence_index];
      g_source_state.next_sequence_index =
          (g_source_state.next_sequence_index + 1) % g_source_state.sequence_paths.size();
      g_source_state.next_frame_deadline_ns = now_ns + g_source_state.frame_interval_ns;
      need_reload = true;
    } else {
      selected_path = g_source_state.loaded_path;
    }
  } else {
    selected_path = g_source_state.descriptor.path;
    struct stat st {};
    if (!stat_path(selected_path, &st)) return false;
    if (g_source_state.loaded_path != selected_path ||
        g_source_state.loaded_mtime != st.st_mtime ||
        g_source_state.loaded_size != st.st_size || g_source_state.frame_bytes.empty()) {
      need_reload = true;
    }
  }

  if (need_reload) {
    std::vector<uint8_t> bytes;
    time_t mtime = 0;
    off_t size = 0;
    if (!load_file_bytes(selected_path, &bytes, &mtime, &size)) {
      LOGE("Failed to load source frame: %s", selected_path.c_str());
      return false;
    }

    int width = g_source_state.descriptor.width;
    int height = g_source_state.descriptor.height;
    if ((width <= 0 || height <= 0) &&
        !infer_dimensions_from_file(selected_path, size, hint_width, hint_height, &width,
                                    &height)) {
      LOGE("Cannot infer source dimensions for %s (size=%jd)", selected_path.c_str(),
           static_cast<intmax_t>(size));
      return false;
    }

    const size_t chroma_width = static_cast<size_t>((width + 1) / 2);
    const size_t chroma_height = static_cast<size_t>((height + 1) / 2);
    const size_t expected_size = static_cast<size_t>(width) * height +
                                 2 * chroma_width * chroma_height;
    if (bytes.size() < expected_size) {
      LOGE("Source frame too small: %s size=%zu expected=%zu", selected_path.c_str(),
           bytes.size(), expected_size);
      return false;
    }

    g_source_state.descriptor.width = width;
    g_source_state.descriptor.height = height;
    g_source_state.frame_bytes = std::move(bytes);
    g_source_state.loaded_path = selected_path;
    g_source_state.loaded_mtime = mtime;
    g_source_state.loaded_size = size;
    g_source_state.load_generation += 1;

    LOGI("Loaded source #%llu path=%s width=%d height=%d format=%s fps=%d sequence=%d",
         static_cast<unsigned long long>(g_source_state.load_generation),
         selected_path.c_str(), g_source_state.descriptor.width,
         g_source_state.descriptor.height,
         source_pixel_format_name(g_source_state.descriptor.format),
         g_source_state.descriptor.fps, g_source_state.descriptor.sequence);
  }

  if (active_descriptor != nullptr) {
    *active_descriptor = g_source_state.descriptor;
  }
  return !g_source_state.frame_bytes.empty();
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

uintptr_t find_exe_base_from_maps() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) return 0;

  char line[1024];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, "/system/bin/cameraserver") == nullptr) continue;
    if (strstr(line, "00000000") == nullptr) continue;

    uintptr_t start = 0;
    if (sscanf(line, "%lx-%*lx", &start) == 1) {
      fclose(fp);
      return start;
    }
  }

  fclose(fp);
  return 0;
}

struct ModuleSearch {
  uintptr_t base = 0;
  char exe_path[PATH_MAX] = {};
};

int find_module_base_cb(struct dl_phdr_info *info, size_t, void *data) {
  auto *search = static_cast<ModuleSearch *>(data);

  const char *name = info->dlpi_name;
  if (name == nullptr || name[0] == '\0') {
    if (search->exe_path[0] == '\0') {
      const ssize_t len = readlink("/proc/self/exe", search->exe_path,
                                   sizeof(search->exe_path) - 1);
      if (len > 0) {
        search->exe_path[len] = '\0';
      }
    }
    name = search->exe_path;
  }

  if (name != nullptr && ends_with(name, kTargetModuleBasename)) {
    search->base = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
  }

  return 0;
}

uintptr_t find_cameraserver_base() {
  ModuleSearch search;
  dl_iterate_phdr(find_module_base_cb, &search);
  if (search.base != 0) return search.base;
  return find_exe_base_from_maps();
}

std::string bytes_to_hex(const uint8_t *data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0xf];
    out[i * 2 + 1] = kHex[data[i] & 0xf];
  }
  return out;
}

bool read_file_bytes(const char *path, std::vector<uint8_t> *out) {
  if (path == nullptr || out == nullptr) return false;
  out->clear();
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (64 * 1024 * 1024)) {
    close(fd);
    return false;
  }
  out->resize(static_cast<size_t>(st.st_size));
  size_t done = 0;
  while (done < out->size()) {
    ssize_t n = read(fd, out->data() + done, out->size() - done);
    if (n <= 0) {
      close(fd);
      out->clear();
      return false;
    }
    done += static_cast<size_t>(n);
  }
  close(fd);
  return true;
}

std::string read_self_build_id() {
  char exe_path[PATH_MAX] = {};
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0) return {};
  exe_path[len] = '\0';

  std::vector<uint8_t> elf;
  if (!read_file_bytes(exe_path, &elf) || elf.size() < sizeof(Elf64_Ehdr)) {
    return {};
  }
  const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(elf.data());
  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr->e_phoff == 0 || ehdr->e_phentsize < sizeof(Elf64_Phdr)) {
    return {};
  }
  const size_t ph_end = static_cast<size_t>(ehdr->e_phoff) +
                        static_cast<size_t>(ehdr->e_phnum) * ehdr->e_phentsize;
  if (ph_end > elf.size()) return {};

  for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(
        elf.data() + static_cast<size_t>(ehdr->e_phoff) +
        static_cast<size_t>(i) * ehdr->e_phentsize);
    if (phdr->p_type != PT_NOTE) continue;
    size_t off = static_cast<size_t>(phdr->p_offset);
    const size_t end = off + static_cast<size_t>(phdr->p_filesz);
    if (end > elf.size()) continue;
    while (off + sizeof(Elf64_Nhdr) <= end) {
      const auto *nhdr = reinterpret_cast<const Elf64_Nhdr *>(elf.data() + off);
      off += sizeof(Elf64_Nhdr);
      const size_t name_off = off;
      const size_t name_end = name_off + nhdr->n_namesz;
      off = (name_end + 3u) & ~size_t{3u};
      const size_t desc_off = off;
      const size_t desc_end = desc_off + nhdr->n_descsz;
      off = (desc_end + 3u) & ~size_t{3u};
      if (name_end > end || desc_end > end) break;
      const char *name = reinterpret_cast<const char *>(elf.data() + name_off);
      if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz >= 3 &&
          memcmp(name, "GNU", 3) == 0 && nhdr->n_descsz > 0) {
        return bytes_to_hex(elf.data() + desc_off, nhdr->n_descsz);
      }
    }
  }
  return {};
}

bool load_self_elf_view(CameraServerElfView *view) {
  if (view == nullptr) return false;
  *view = CameraServerElfView{};

  char exe_path[PATH_MAX] = {};
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0) return false;
  exe_path[len] = '\0';
  if (!read_file_bytes(exe_path, &view->bytes) ||
      view->bytes.size() < sizeof(Elf64_Ehdr)) {
    return false;
  }

  const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(view->bytes.data());
  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr->e_shoff == 0 || ehdr->e_shstrndx == SHN_UNDEF ||
      ehdr->e_shentsize < sizeof(Elf64_Shdr)) {
    return false;
  }
  const size_t sh_end = static_cast<size_t>(ehdr->e_shoff) +
                        static_cast<size_t>(ehdr->e_shnum) * ehdr->e_shentsize;
  if (sh_end > view->bytes.size()) return false;
  const auto *shstr = reinterpret_cast<const Elf64_Shdr *>(
      view->bytes.data() + static_cast<size_t>(ehdr->e_shoff) +
      static_cast<size_t>(ehdr->e_shstrndx) * ehdr->e_shentsize);
  if (static_cast<size_t>(shstr->sh_offset + shstr->sh_size) > view->bytes.size()) {
    return false;
  }
  const char *names = reinterpret_cast<const char *>(view->bytes.data() + shstr->sh_offset);
  for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
    const auto *sh = reinterpret_cast<const Elf64_Shdr *>(
        view->bytes.data() + static_cast<size_t>(ehdr->e_shoff) +
        static_cast<size_t>(i) * ehdr->e_shentsize);
    if (sh->sh_name >= shstr->sh_size) continue;
    const char *name = names + sh->sh_name;
    if (static_cast<size_t>(sh->sh_offset + sh->sh_size) > view->bytes.size()) continue;
    if (strcmp(name, ".text") == 0) {
      view->text = ElfRange{static_cast<size_t>(sh->sh_offset),
                            static_cast<uintptr_t>(sh->sh_addr),
                            static_cast<size_t>(sh->sh_size)};
    } else if (strcmp(name, ".rodata") == 0) {
      view->rodata = ElfRange{static_cast<size_t>(sh->sh_offset),
                              static_cast<uintptr_t>(sh->sh_addr),
                              static_cast<size_t>(sh->sh_size)};
    }
  }
  return view->text.size > 0 && view->rodata.size > 0;
}

std::vector<uintptr_t> find_rodata_string_vaddrs(const CameraServerElfView &view,
                                                 const char *needle) {
  std::vector<uintptr_t> out;
  if (needle == nullptr || needle[0] == '\0' || view.rodata.size == 0) return out;
  const uint8_t *base = view.bytes.data() + view.rodata.file_offset;
  const size_t len = strlen(needle);
  if (len > view.rodata.size) return out;
  for (size_t off = 0; off + len <= view.rodata.size; ++off) {
    if (memcmp(base + off, needle, len) == 0) {
      out.push_back(view.rodata.vaddr + off);
    }
  }
  return out;
}

uint32_t read_u32_le_unaligned(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int64_t sign_extend64(uint64_t value, int bits) {
  const uint64_t sign = 1ULL << (bits - 1);
  return static_cast<int64_t>((value ^ sign) - sign);
}

bool decode_adrp_target(uint32_t insn, uintptr_t pc, int *rd, uintptr_t *page_out) {
  if ((insn & 0x9f000000u) != 0x90000000u || rd == nullptr || page_out == nullptr) {
    return false;
  }
  const uint64_t immlo = (insn >> 29) & 0x3u;
  const uint64_t immhi = (insn >> 5) & 0x7ffffu;
  const int64_t imm = sign_extend64((immhi << 2) | immlo, 21) << 12;
  *rd = static_cast<int>(insn & 0x1fu);
  *page_out = static_cast<uintptr_t>((pc & ~uintptr_t{0xfff}) + imm);
  return true;
}

bool decode_add_imm_target(uint32_t insn, int expected_rn, uintptr_t page,
                           uintptr_t *target_out) {
  if ((insn & 0xff000000u) != 0x91000000u || target_out == nullptr) return false;
  const int rn = static_cast<int>((insn >> 5) & 0x1fu);
  if (rn != expected_rn) return false;
  uint64_t imm = (insn >> 10) & 0xfffu;
  const uint32_t shift = (insn >> 22) & 0x3u;
  if (shift == 1) imm <<= 12;
  else if (shift != 0) return false;
  *target_out = static_cast<uintptr_t>(page + imm);
  return true;
}

std::vector<uintptr_t> find_adrp_add_xrefs(const CameraServerElfView &view,
                                           uintptr_t target_vaddr) {
  std::vector<uintptr_t> xrefs;
  const uint8_t *text = view.bytes.data() + view.text.file_offset;
  for (size_t off = 0; off + 8 <= view.text.size; off += 4) {
    const uintptr_t pc = view.text.vaddr + off;
    int rd = -1;
    uintptr_t page = 0;
    const uint32_t adrp = read_u32_le_unaligned(text + off);
    if (!decode_adrp_target(adrp, pc, &rd, &page)) continue;
    for (size_t look = 4; look <= 16 && off + look + 4 <= view.text.size; look += 4) {
      uintptr_t computed = 0;
      const uint32_t add = read_u32_le_unaligned(text + off + look);
      if (decode_add_imm_target(add, rd, page, &computed) && computed == target_vaddr) {
        xrefs.push_back(pc);
        break;
      }
    }
  }
  return xrefs;
}

bool is_sub_sp_sp_imm(uint32_t insn) {
  return (insn & 0xffc003ffu) == 0xd10003ffu;
}

uintptr_t find_function_start_before(const CameraServerElfView &view, uintptr_t xref) {
  if (xref < view.text.vaddr || xref >= view.text.vaddr + view.text.size) return 0;
  size_t off = static_cast<size_t>(xref - view.text.vaddr);
  off &= ~size_t{3u};
  const size_t min_off = off > 0x5000 ? off - 0x5000 : 0;
  const uint8_t *text = view.bytes.data() + view.text.file_offset;
  for (size_t cur = off; cur >= min_off + 4; cur -= 4) {
    const uint32_t insn = read_u32_le_unaligned(text + cur);
    if (is_sub_sp_sp_imm(insn)) return view.text.vaddr + cur;
    if (cur < 4) break;
  }
  return 0;
}

uintptr_t resolve_function_by_anchor_strings(const CameraServerElfView &view,
                                             const std::vector<const char *> &anchors,
                                             const char *label) {
  struct Candidate {
    uintptr_t start = 0;
    int hits = 0;
  };
  std::vector<Candidate> candidates;

  for (const char *anchor : anchors) {
    const auto str_vaddrs = find_rodata_string_vaddrs(view, anchor);
    for (uintptr_t str_vaddr : str_vaddrs) {
      const auto xrefs = find_adrp_add_xrefs(view, str_vaddr);
      for (uintptr_t xref : xrefs) {
        const uintptr_t start = find_function_start_before(view, xref);
        if (start == 0) continue;
        auto it = std::find_if(candidates.begin(), candidates.end(),
                               [&](const Candidate &c) { return c.start == start; });
        if (it == candidates.end()) candidates.push_back(Candidate{start, 1});
        else it->hits += 1;
      }
    }
  }

  if (candidates.empty()) {
    LOGW("scanner: %s no candidates", label != nullptr ? label : "(null)");
    return 0;
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
              if (a.hits != b.hits) return a.hits > b.hits;
              return a.start < b.start;
            });
  if (candidates.size() > 1 && candidates[0].hits == candidates[1].hits) {
    LOGE("scanner: %s ambiguous top candidates %#lx and %#lx hits=%d",
         label != nullptr ? label : "(null)",
         static_cast<unsigned long>(candidates[0].start),
         static_cast<unsigned long>(candidates[1].start), candidates[0].hits);
    return 0;
  }
  LOGI("scanner: %s resolved offset=%#lx hits=%d candidates=%zu",
       label != nullptr ? label : "(null)",
       static_cast<unsigned long>(candidates[0].start), candidates[0].hits,
       candidates.size());
  return candidates[0].start;
}

bool scan_hook_offsets(HookOffsetConfig *cfg) {
  if (cfg == nullptr) return false;
  CameraServerElfView view;
  if (!load_self_elf_view(&view)) {
    LOGE("scanner: failed to load self ELF view");
    return false;
  }

  HookOffsetConfig scanned = *cfg;
  scanned.from_file = false;
  scanned.enable_create_configured_surface = false;
  scanned.create_stream = resolve_function_by_anchor_strings(
      view,
      {
          "Deferred consumer stream creation only support IMPLEMENTATION_DEFINED format",
          "Can't reconfigure device for new stream",
          "Number of consumers cannot be smaller than 1",
      },
      "Camera3Device::createStream");
  scanned.return_buffer_checked_locked = resolve_function_by_anchor_strings(
      view,
      {
          "surface_id %zu for Camera3OutputStream should be 0!",
          "returnBufferCheckedLocked",
      },
      "Camera3OutputStream::returnBufferCheckedLocked");

  scanned.enable_create_stream = scanned.create_stream != 0;
  scanned.enable_return_buffer_checked_locked =
      scanned.return_buffer_checked_locked != 0;

  if (!scanned.enable_create_stream || !scanned.enable_return_buffer_checked_locked) {
    LOGE("scanner: incomplete createStream=%#lx returnBuffer=%#lx",
         static_cast<unsigned long>(scanned.create_stream),
         static_cast<unsigned long>(scanned.return_buffer_checked_locked));
    return false;
  }

  *cfg = scanned;
  return true;
}

std::string format_offset_config_text(const HookOffsetConfig &cfg,
                                      const std::string &build_id) {
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "# awesomeCAM auto-generated cameraserver offsets\n"
           "build_id=%s\n"
           "create_configured_surface=0x%lx\n"
           "create_stream=0x%lx\n"
           "return_buffer_checked_locked=0x%lx\n"
           "enable_create_configured_surface=%d\n"
           "enable_create_stream=%d\n"
           "enable_return_buffer=1\n",
           build_id.empty() ? "*" : build_id.c_str(),
           static_cast<unsigned long>(cfg.create_configured_surface),
           static_cast<unsigned long>(cfg.create_stream),
           static_cast<unsigned long>(cfg.return_buffer_checked_locked),
           cfg.enable_create_configured_surface ? 1 : 0,
           cfg.enable_create_stream ? 1 : 0);
  return std::string(buf);
}

bool save_hook_offset_config_file(const HookOffsetConfig &cfg,
                                  const std::string &build_id) {
  const std::string text = format_offset_config_text(cfg, build_id);
  const int fd = open(kOffsetConfigPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0666);
  if (fd < 0) {
    LOGE("scanner: save %s failed errno=%d (%s); config follows:\n%s",
         kOffsetConfigPath, errno, strerror(errno), text.c_str());
    return false;
  }
  const char *p = text.c_str();
  size_t left = text.size();
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) {
      const int err = errno;
      close(fd);
      LOGE("scanner: write %s failed errno=%d (%s)", kOffsetConfigPath, err,
           strerror(err));
      return false;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  fchmod(fd, 0666);
  close(fd);
  LOGI("scanner: saved offsets to %s", kOffsetConfigPath);
  return true;
}

bool parse_bool_config(const std::string &value, bool fallback) {
  std::string v;
  v.reserve(value.size());
  for (char c : value) v.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

bool parse_uintptr_config(const std::string &value, uintptr_t *out) {
  if (out == nullptr) return false;
  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = strtoull(value.c_str(), &end, 0);
  if (errno != 0 || end == value.c_str()) return false;
  *out = static_cast<uintptr_t>(parsed);
  return true;
}

bool load_hook_offset_config_file(const std::string &current_build_id,
                                  HookOffsetConfig *cfg) {
  if (cfg == nullptr) return false;
  FILE *fp = fopen(kOffsetConfigPath, "re");
  if (fp == nullptr) return false;

  HookOffsetConfig parsed = *cfg;
  parsed.from_file = true;
  bool saw_key = false;
  char line[512];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    std::string s = trim_copy(line);
    if (s.empty() || s[0] == '#') continue;
    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    saw_key = true;
    const std::string key = trim_copy(s.substr(0, eq));
    const std::string value = trim_copy(s.substr(eq + 1));
    if (key == "build_id") {
      parsed.expected_build_id = value;
    } else if (key == "create_configured_surface") {
      (void)parse_uintptr_config(value, &parsed.create_configured_surface);
    } else if (key == "create_stream") {
      (void)parse_uintptr_config(value, &parsed.create_stream);
    } else if (key == "return_buffer_checked_locked" || key == "return_buffer") {
      (void)parse_uintptr_config(value, &parsed.return_buffer_checked_locked);
    } else if (key == "enable_create_configured_surface") {
      parsed.enable_create_configured_surface =
          parse_bool_config(value, parsed.enable_create_configured_surface);
    } else if (key == "enable_create_stream") {
      parsed.enable_create_stream = parse_bool_config(value, parsed.enable_create_stream);
    } else if (key == "enable_return_buffer_checked_locked" ||
               key == "enable_return_buffer") {
      parsed.enable_return_buffer_checked_locked =
          parse_bool_config(value, parsed.enable_return_buffer_checked_locked);
    }
  }
  fclose(fp);

  if (!saw_key) return false;

  if (!parsed.expected_build_id.empty() && parsed.expected_build_id != "*" &&
      (current_build_id.empty() || parsed.expected_build_id != current_build_id)) {
    LOGE("offset config build_id mismatch config=%s current=%s; refusing stale offsets",
         parsed.expected_build_id.c_str(), current_build_id.c_str());
    return false;
  }
  *cfg = parsed;
  return true;
}

HookOffsetConfig resolve_hook_offset_config() {
  HookOffsetConfig cfg;
  const std::string build_id = read_self_build_id();
  LOGI("cameraserver build_id=%s", build_id.empty() ? "(unknown)" : build_id.c_str());

  if (load_hook_offset_config_file(build_id, &cfg)) {
    LOGI("using hook offsets from %s build_id=%s createSurface=%#lx createStream=%#lx returnBuffer=%#lx enable={%d,%d,%d}",
         kOffsetConfigPath, cfg.expected_build_id.c_str(),
         static_cast<unsigned long>(cfg.create_configured_surface),
         static_cast<unsigned long>(cfg.create_stream),
         static_cast<unsigned long>(cfg.return_buffer_checked_locked),
         cfg.enable_create_configured_surface ? 1 : 0,
         cfg.enable_create_stream ? 1 : 0,
         cfg.enable_return_buffer_checked_locked ? 1 : 0);
    return cfg;
  }

  if (build_id == kKnownCameraServerBuildId) {
    LOGI("using built-in hook offsets for known cameraserver build_id=%s",
         build_id.c_str());
    return cfg;
  }

  LOGW("unknown cameraserver build_id=%s; trying signature scanner",
       build_id.empty() ? "(unknown)" : build_id.c_str());
  if (scan_hook_offsets(&cfg)) {
    cfg.expected_build_id = build_id.empty() ? "*" : build_id;
    LOGI("using scanner hook offsets createStream=%#lx returnBuffer=%#lx",
         static_cast<unsigned long>(cfg.create_stream),
         static_cast<unsigned long>(cfg.return_buffer_checked_locked));
    (void)save_hook_offset_config_file(cfg, build_id);
    return cfg;
  }

  cfg.enable_create_configured_surface = false;
  cfg.enable_create_stream = false;
  cfg.enable_return_buffer_checked_locked = false;
  LOGE("unknown cameraserver build_id=%s and no valid %s; hooks disabled to avoid OTA crash",
       build_id.empty() ? "(unknown)" : build_id.c_str(), kOffsetConfigPath);
  LOGE("create %s with: build_id=%s create_stream=0x... return_buffer_checked_locked=0x... enable_create_stream=1 enable_return_buffer=1",
       kOffsetConfigPath, build_id.empty() ? "*" : build_id.c_str());
  return cfg;
}

void remember_stream_record(int stream_id, uint32_t width, uint32_t height, int format,
                            int32_t data_space, uint64_t consumer_usage,
                            int stream_set_id, bool deferred, bool shared,
                            bool multi_resolution, int timestamp_base,
                            int32_t color_space, bool use_readout_timestamp) {
  if (stream_id < 0) return;

  if (width > 0 && height > 0 && (format == 0x22 || format == 0x23)) {
    awesomecam::RegisterReadyFrameTarget(static_cast<int32_t>(width),
                                         static_cast<int32_t>(height));
  }

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

void register_ready_target_if_supported(uint32_t width, uint32_t height, int format) {
  if (width == 0 || height == 0) return;
  if (format != kHalPixelFormatImplementationDefined &&
      format != kHalPixelFormatYcbcr420888) {
    return;
  }
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

bool build_source_i420_view_locked(const SourceDescriptor &descriptor,
                                   const std::vector<uint8_t> &frame_bytes,
                                   std::vector<uint8_t> *scratch,
                                   I420View *view) {
  if (view == nullptr || descriptor.width <= 0 || descriptor.height <= 0) return false;

  const size_t y_size = static_cast<size_t>(descriptor.width) * descriptor.height;
  const size_t chroma_width = chroma_width_for(descriptor.width);
  const size_t chroma_height = chroma_height_for(descriptor.height);
  const size_t chroma_size = chroma_width * chroma_height;
  const size_t required_size = y_size + 2 * chroma_size;
  if (frame_bytes.size() < required_size) return false;

  if (descriptor.format == SourcePixelFormat::kI420) {
    view->width = descriptor.width;
    view->height = descriptor.height;
    view->y = frame_bytes.data();
    view->u = frame_bytes.data() + y_size;
    view->v = frame_bytes.data() + y_size + chroma_size;
    view->y_stride = descriptor.width;
    view->u_stride = static_cast<int>(chroma_width);
    view->v_stride = static_cast<int>(chroma_width);
    return true;
  }

  if (scratch == nullptr) return false;
  scratch->resize(required_size);
  uint8_t *dst_y = scratch->data();
  uint8_t *dst_u = dst_y + y_size;
  uint8_t *dst_v = dst_u + chroma_size;
  memcpy(dst_y, frame_bytes.data(), y_size);

  const uint8_t *src_uv = frame_bytes.data() + y_size;
  for (size_t y = 0; y < chroma_height; ++y) {
    const uint8_t *src_row = src_uv + y * chroma_width * 2;
    uint8_t *u_row = dst_u + y * chroma_width;
    uint8_t *v_row = dst_v + y * chroma_width;
    for (size_t x = 0; x < chroma_width; ++x) {
      const uint8_t a = src_row[x * 2 + 0];
      const uint8_t b = src_row[x * 2 + 1];
      if (descriptor.format == SourcePixelFormat::kNV12) {
        u_row[x] = a;
        v_row[x] = b;
      } else {
        v_row[x] = a;
        u_row[x] = b;
      }
    }
  }

  view->width = descriptor.width;
  view->height = descriptor.height;
  view->y = dst_y;
  view->u = dst_u;
  view->v = dst_v;
  view->y_stride = descriptor.width;
  view->u_stride = static_cast<int>(chroma_width);
  view->v_stride = static_cast<int>(chroma_width);
  return true;
}

void scale_plane_crop_nn(const uint8_t *src, int src_stride, int crop_x, int crop_y,
                         int crop_w, int crop_h, uint8_t *dst, int dst_stride,
                         int dst_w, int dst_h) {
  for (int y = 0; y < dst_h; ++y) {
    const int src_y = crop_y + (y * crop_h) / std::max(1, dst_h);
    const uint8_t *src_row = src + src_y * src_stride;
    uint8_t *dst_row = dst + y * dst_stride;
    for (int x = 0; x < dst_w; ++x) {
      const int src_x = crop_x + (x * crop_w) / std::max(1, dst_w);
      dst_row[x] = src_row[src_x];
    }
  }
}

void compute_center_crop(int src_w, int src_h, int dst_w, int dst_h, int *crop_x,
                         int *crop_y, int *crop_w, int *crop_h) {
  if (crop_x == nullptr || crop_y == nullptr || crop_w == nullptr || crop_h == nullptr) {
    return;
  }

  int out_x = 0;
  int out_y = 0;
  int out_w = src_w;
  int out_h = src_h;

  if (src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0) {
    const int64_t lhs = static_cast<int64_t>(src_w) * dst_h;
    const int64_t rhs = static_cast<int64_t>(dst_w) * src_h;
    if (lhs > rhs) {
      out_w = static_cast<int>((static_cast<int64_t>(src_h) * dst_w) / dst_h);
      out_x = (src_w - out_w) / 2;
    } else if (lhs < rhs) {
      out_h = static_cast<int>((static_cast<int64_t>(src_w) * dst_h) / dst_w);
      out_y = (src_h - out_h) / 2;
    }
  }

  out_x &= ~1;
  out_y &= ~1;
  if ((out_w & 1) != 0 && out_w > 1) --out_w;
  if ((out_h & 1) != 0 && out_h > 1) --out_h;
  out_w = std::max(2, out_w);
  out_h = std::max(2, out_h);

  *crop_x = out_x;
  *crop_y = out_y;
  *crop_w = out_w;
  *crop_h = out_h;
}

bool build_scaled_i420_locked(const SourceDescriptor &descriptor,
                              const std::vector<uint8_t> &frame_bytes, int dst_width,
                              int dst_height, std::vector<uint8_t> *scaled_i420) {
  if (scaled_i420 == nullptr || dst_width <= 0 || dst_height <= 0) return false;

  std::vector<uint8_t> source_scratch;
  I420View src;
  if (!build_source_i420_view_locked(descriptor, frame_bytes, &source_scratch, &src)) {
    return false;
  }

  const size_t dst_y_size = static_cast<size_t>(dst_width) * dst_height;
  const size_t dst_chroma_width = chroma_width_for(dst_width);
  const size_t dst_chroma_height = chroma_height_for(dst_height);
  const size_t dst_chroma_size = dst_chroma_width * dst_chroma_height;
  scaled_i420->resize(dst_y_size + 2 * dst_chroma_size);

  uint8_t *dst_y = scaled_i420->data();
  uint8_t *dst_u = dst_y + dst_y_size;
  uint8_t *dst_v = dst_u + dst_chroma_size;

  int crop_x = 0;
  int crop_y = 0;
  int crop_w = src.width;
  int crop_h = src.height;
  compute_center_crop(src.width, src.height, dst_width, dst_height, &crop_x, &crop_y,
                      &crop_w, &crop_h);

  const uint8_t *src_y = src.y + static_cast<size_t>(crop_y) * src.y_stride + crop_x;
  const uint8_t *src_u =
      src.u + static_cast<size_t>(crop_y / 2) * src.u_stride + (crop_x / 2);
  const uint8_t *src_v =
      src.v + static_cast<size_t>(crop_y / 2) * src.v_stride + (crop_x / 2);
  constexpr int kLibYuvFilterBilinear = 2;
  if (awesomecam::LibYuvI420Scale(src_y, src.y_stride,
                                  src_u, src.u_stride,
                                  src_v, src.v_stride,
                                  crop_w, crop_h,
                                  dst_y, dst_width,
                                  dst_u, static_cast<int>(dst_chroma_width),
                                  dst_v, static_cast<int>(dst_chroma_width),
                                  dst_width, dst_height,
                                  kLibYuvFilterBilinear)) {
    return true;
  }

  scale_plane_crop_nn(src.y, src.y_stride, crop_x, crop_y, crop_w, crop_h, dst_y,
                      dst_width, dst_width, dst_height);
  scale_plane_crop_nn(src.u, src.u_stride, crop_x / 2, crop_y / 2, crop_w / 2,
                      crop_h / 2, dst_u, static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  scale_plane_crop_nn(src.v, src.v_stride, crop_x / 2, crop_y / 2, crop_w / 2,
                      crop_h / 2, dst_v, static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  return true;
}

const ScaledFrameCacheEntry *find_scaled_frame_cache_entry_locked(
    ScaledFrameSourceKind source_kind, uint64_t source_generation, int dst_width,
    int dst_height) {
  if (g_scaled_frame_cache.source_kind != source_kind ||
      g_scaled_frame_cache.source_generation != source_generation) {
    return nullptr;
  }
  for (const auto &entry : g_scaled_frame_cache.entries) {
    if (entry.dst_width == dst_width && entry.dst_height == dst_height) {
      return &entry;
    }
  }
  return nullptr;
}

void reset_scaled_frame_cache_locked(ScaledFrameSourceKind source_kind,
                                     uint64_t source_generation) {
  if (g_scaled_frame_cache.source_kind == source_kind &&
      g_scaled_frame_cache.source_generation == source_generation) {
    return;
  }
  g_scaled_frame_cache.source_kind = source_kind;
  g_scaled_frame_cache.source_generation = source_generation;
  g_scaled_frame_cache.entries.clear();
}

const ScaledFrameCacheEntry *store_scaled_frame_cache_entry_locked(
    ScaledFrameSourceKind source_kind, uint64_t source_generation, int dst_width,
    int dst_height, const SourceDescriptor &source_descriptor,
    std::vector<uint8_t> &&scaled_i420) {
  reset_scaled_frame_cache_locked(source_kind, source_generation);
  for (auto &entry : g_scaled_frame_cache.entries) {
    if (entry.dst_width == dst_width && entry.dst_height == dst_height) {
      entry.source_descriptor = source_descriptor;
      entry.scaled_i420 = std::move(scaled_i420);
      return &entry;
    }
  }
  g_scaled_frame_cache.entries.push_back(
      ScaledFrameCacheEntry{dst_width, dst_height, source_descriptor,
                            std::move(scaled_i420)});
  return &g_scaled_frame_cache.entries.back();
}

bool load_latest_scaled_i420_for_dst(int dst_width, int dst_height,
                                     SourceDescriptor *source_descriptor,
                                     const std::vector<uint8_t> **scaled_i420) {
  if (source_descriptor == nullptr || scaled_i420 == nullptr || dst_width <= 0 ||
      dst_height <= 0) {
    return false;
  }

  *source_descriptor = SourceDescriptor{};
  *scaled_i420 = nullptr;

  awesomecam::BinderFrameState binder_state;
  if (awesomecam::PeekLatestBinderFrameState(&binder_state) &&
      binder_state.format == awesomecam::kFrameFormatI420) {
    if (const auto *cached = find_scaled_frame_cache_entry_locked(
            ScaledFrameSourceKind::kBinder, binder_state.generation, dst_width,
            dst_height)) {
      *source_descriptor = cached->source_descriptor;
      *scaled_i420 = &cached->scaled_i420;
      return true;
    }

    awesomecam::BinderFrameCopy binder_frame;
    if (awesomecam::CopyLatestBinderFrame(&binder_frame) &&
        binder_frame.format == awesomecam::kFrameFormatI420) {
      SourceDescriptor binder_descriptor;
      binder_descriptor.valid = true;
      binder_descriptor.sequence = false;
      binder_descriptor.path = "binder://Video2CameraService";
      binder_descriptor.width = binder_frame.width;
      binder_descriptor.height = binder_frame.height;
      binder_descriptor.fps = kDefaultSourceFps;
      binder_descriptor.format = SourcePixelFormat::kI420;

      std::vector<uint8_t> built_i420;
      if (!build_scaled_i420_locked(binder_descriptor, binder_frame.bytes, dst_width,
                                    dst_height, &built_i420)) {
        LOGE("Failed to scale binder frame gen=%llu %dx%d for %dx%d",
             static_cast<unsigned long long>(binder_frame.generation),
             binder_frame.width, binder_frame.height, dst_width, dst_height);
      } else {
        const auto *cached = store_scaled_frame_cache_entry_locked(
            ScaledFrameSourceKind::kBinder, binder_frame.generation, dst_width,
            dst_height, binder_descriptor, std::move(built_i420));
        *source_descriptor = cached->source_descriptor;
        *scaled_i420 = &cached->scaled_i420;
        return true;
      }
    }
  }

  std::lock_guard<std::mutex> source_lock(g_source_state.mutex);
  if (!ensure_source_frame_locked(dst_width, dst_height, source_descriptor)) {
    return false;
  }

  if (const auto *cached = find_scaled_frame_cache_entry_locked(
          ScaledFrameSourceKind::kLocal, g_source_state.load_generation, dst_width,
          dst_height)) {
    *source_descriptor = cached->source_descriptor;
    *scaled_i420 = &cached->scaled_i420;
    return true;
  }

  std::vector<uint8_t> built_i420;
  if (!build_scaled_i420_locked(*source_descriptor, g_source_state.frame_bytes, dst_width,
                                dst_height, &built_i420)) {
    LOGE("Failed to build scaled I420 for %dx%d from %s", dst_width, dst_height,
         source_pixel_format_name(source_descriptor->format));
    return false;
  }

  const auto *cached = store_scaled_frame_cache_entry_locked(
      ScaledFrameSourceKind::kLocal, g_source_state.load_generation, dst_width,
      dst_height, *source_descriptor, std::move(built_i420));
  *source_descriptor = cached->source_descriptor;
  *scaled_i420 = &cached->scaled_i420;
  return true;
}

void copy_plane_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src, int src_stride,
                     int row_width, int rows) {
  for (int y = 0; y < rows; ++y) {
    memcpy(dst + y * dst_stride, src + y * src_stride, static_cast<size_t>(row_width));
  }
}

bool write_i420_to_ycbcr(const std::vector<uint8_t> &scaled_i420, int width, int height,
                         const android_ycbcr &layout) {
  if (width <= 0 || height <= 0 || layout.y == nullptr || layout.cb == nullptr ||
      layout.cr == nullptr) {
    return false;
  }

  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_width = chroma_width_for(width);
  const size_t chroma_height = chroma_height_for(height);
  const size_t chroma_size = chroma_width * chroma_height;
  if (scaled_i420.size() < y_size + 2 * chroma_size) return false;

  const uint8_t *src_y = scaled_i420.data();
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

bool wait_fence_if_valid(int fence_fd, const char *where) {
  if (fence_fd < 0) return true;

  const uint64_t start_ns = monotonic_time_ns();
  pollfd pfd{};
  pfd.fd = fence_fd;
  pfd.events = POLLIN;

  int rc = -1;
  do {
    rc = poll(&pfd, 1, 1000);
  } while (rc < 0 && errno == EINTR);

  const uint64_t done_ns = monotonic_time_ns();
  const uint64_t count =
      g_fence_wait_count.fetch_add(1, std::memory_order_relaxed) + 1;

  if (rc == 0) {
    const uint64_t timeout_count =
        g_fence_wait_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (timeout_count <= 20 || (timeout_count % 120) == 0) {
      LOGW("%s fence wait timeout #%llu fd=%d waited=%.3fms",
           where != nullptr ? where : "replace",
           static_cast<unsigned long long>(timeout_count), fence_fd,
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

bool write_i420_to_contiguous_nv(const std::vector<uint8_t> &scaled_i420,
                                 int width, int height, void *vaddr,
                                 int stride_bytes, bool nv21) {
  if (width <= 0 || height <= 0 || vaddr == nullptr) return false;
  if (stride_bytes <= 0) stride_bytes = width;
  if (stride_bytes < width) return false;

  const int chroma_width = static_cast<int>(chroma_width_for(width));
  const int chroma_height = static_cast<int>(chroma_height_for(height));
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_size = static_cast<size_t>(chroma_width) * chroma_height;
  if (scaled_i420.size() < y_size + 2 * chroma_size) return false;

  const uint8_t *src_y = scaled_i420.data();
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

bool try_write_i420_to_ahardwarebuffer_planes(void *graphic_buffer,
                                              const std::vector<uint8_t> &scaled_i420,
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
    replaced = write_i420_to_ycbcr(scaled_i420, width, height, layout);
  } else if (planes.planeCount >= 2 && planes.planes[0].data != nullptr &&
             planes.planes[1].data != nullptr) {
    const size_t y_size = static_cast<size_t>(width) * height;
    const int chroma_width = static_cast<int>(chroma_width_for(width));
    const size_t chroma_size =
        static_cast<size_t>(chroma_width) * static_cast<size_t>(chroma_height_for(height));
    if (scaled_i420.size() >= y_size + 2 * chroma_size) {
      const uint8_t *src_y = scaled_i420.data();
      const uint8_t *src_u = src_y + y_size;
      const uint8_t *src_v = src_u + chroma_size;
      const bool nv21 = prefer_raw_nv21_output();
      replaced = nv21
          ? awesomecam::LibYuvI420ToNV21(
                src_y, width, src_u, chroma_width, src_v, chroma_width,
                static_cast<uint8_t *>(planes.planes[0].data),
                static_cast<int>(planes.planes[0].rowStride),
                static_cast<uint8_t *>(planes.planes[1].data),
                static_cast<int>(planes.planes[1].rowStride), width, height)
          : awesomecam::LibYuvI420ToNV12(
                src_y, width, src_u, chroma_width, src_v, chroma_width,
                static_cast<uint8_t *>(planes.planes[0].data),
                static_cast<int>(planes.planes[0].rowStride),
                static_cast<uint8_t *>(planes.planes[1].data),
                static_cast<int>(planes.planes[1].rowStride), width, height);
    }
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


bool try_replace_camera3_frame(
    const android::camera3::camera_stream_buffer &buffer, int32_t surface_id,
    StreamRecord *matched_stream) {
  if (buffer.stream == nullptr || buffer.buffer == nullptr) return false;
  if (buffer.stream->format != kHalPixelFormatYcbcr420888 &&
      buffer.stream->format != kHalPixelFormatImplementationDefined) {
    return false;
  }
  if (!resolve_graphic_buffer_api()) return false;

  const uint64_t total_start_ns = monotonic_time_ns();
  SourceDescriptor source_descriptor;
  const std::vector<uint8_t> *scaled_i420 = nullptr;
  std::unique_lock<std::mutex> fallback_process_lock(g_frame_process_mutex,
                                                     std::defer_lock);
  const uint64_t get_start_ns = monotonic_time_ns();
  std::shared_ptr<const awesomecam::ReadyI420Frame> ready_frame =
      awesomecam::GetReadyI420Frame(static_cast<int32_t>(buffer.stream->width),
                                    static_cast<int32_t>(buffer.stream->height));
  const uint64_t get_done_ns = monotonic_time_ns();
  uint64_t load_start_ns = get_done_ns;
  uint64_t load_done_ns = get_done_ns;
  const bool ready_path = ready_frame != nullptr && !ready_frame->bytes.empty();
  if (ready_path) {
    scaled_i420 = &ready_frame->bytes;
    source_descriptor.valid = true;
    source_descriptor.sequence = false;
    source_descriptor.path = "ready://NativePlayback";
    source_descriptor.width = ready_frame->width;
    source_descriptor.height = ready_frame->height;
    source_descriptor.fps = kDefaultSourceFps;
    source_descriptor.format = SourcePixelFormat::kI420;
  } else {
    load_start_ns = monotonic_time_ns();
    fallback_process_lock.lock();
    if (!load_latest_scaled_i420_for_dst(static_cast<int>(buffer.stream->width),
                                         static_cast<int>(buffer.stream->height),
                                         &source_descriptor, &scaled_i420) ||
        scaled_i420 == nullptr) {
      return false;
    }
    load_done_ns = monotonic_time_ns();
  }

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
  bool replaced = false;

  android_ycbcr layout{};
  const uint64_t lock_start_ns = monotonic_time_ns();
  const int lock_rc = g_graphic_buffer_api.lock_ycbcr(graphic_buffer,
                                                      kGraphicBufferCpuLockUsage,
                                                      &layout);
  uint64_t lock_done_ns = monotonic_time_ns();
  uint64_t write_start_ns = lock_done_ns;
  uint64_t write_done_ns = lock_done_ns;
  uint64_t unlock_start_ns = lock_done_ns;
  uint64_t unlock_done_ns = lock_done_ns;
  if (lock_rc != 0) {
    const uint64_t fail_count =
        g_lock_ycbcr_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fail_count <= 20 || (fail_count % 120) == 0) {
      LOGE("GraphicBuffer::lockYCbCr failed #%llu rc=%d streamId=%d for %ux%u fmt=%#x surfaceId=%d",
           static_cast<unsigned long long>(fail_count), lock_rc,
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height, buffer.stream->format,
           surface_id);
    }

    write_start_ns = monotonic_time_ns();
    replaced = try_write_i420_to_ahardwarebuffer_planes(
        graphic_buffer, *scaled_i420, static_cast<int>(buffer.stream->width),
        static_cast<int>(buffer.stream->height), "returnBufferCheckedLocked");
    write_done_ns = monotonic_time_ns();

    if (!replaced) {
      void *raw_vaddr = nullptr;
      int raw_bpp = 0;
      int raw_stride = 0;
      const uint64_t raw_lock_start_ns = monotonic_time_ns();
      const int raw_rc = g_graphic_buffer_api.lock(graphic_buffer,
                                                   kGraphicBufferCpuLockUsage,
                                                   &raw_vaddr, &raw_bpp, &raw_stride);
      const uint64_t raw_lock_done_ns = monotonic_time_ns();
      lock_done_ns = raw_lock_done_ns;
      if (raw_rc == 0 && raw_vaddr != nullptr) {
        const uint64_t raw_success =
            g_raw_lock_success_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool nv21 = prefer_raw_nv21_output();
        write_start_ns = monotonic_time_ns();
        replaced = write_i420_to_contiguous_nv(*scaled_i420,
                                               static_cast<int>(buffer.stream->width),
                                               static_cast<int>(buffer.stream->height),
                                               raw_vaddr, raw_stride, nv21);
        write_done_ns = monotonic_time_ns();
        unlock_start_ns = write_done_ns;
        const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
        unlock_done_ns = monotonic_time_ns();
        if (raw_success <= 20 || (raw_success % 120) == 0 || replaced) {
          LOGI("GraphicBuffer::raw lock fallback #%llu streamId=%d rc=%d %ux%u fmt=%#x bpp=%d stride=%d nv21=%d replaced=%d lock=%.3fms",
               static_cast<unsigned long long>(raw_success),
               matched_stream != nullptr ? matched_stream->stream_id : -1, raw_rc,
               buffer.stream->width, buffer.stream->height, buffer.stream->format,
               raw_bpp, raw_stride, nv21 ? 1 : 0, replaced ? 1 : 0,
               ns_to_ms(raw_lock_done_ns - raw_lock_start_ns));
        }
        if (unlock_rc != 0) {
          LOGW("GraphicBuffer::raw unlock failed rc=%d", unlock_rc);
        }
      } else {
        const uint64_t raw_fail =
            g_raw_lock_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (raw_fail <= 20 || (raw_fail % 120) == 0) {
          LOGE("GraphicBuffer::raw lock fallback failed #%llu rc=%d streamId=%d %ux%u fmt=%#x bpp=%d stride=%d vaddr=%p",
               static_cast<unsigned long long>(raw_fail), raw_rc,
               matched_stream != nullptr ? matched_stream->stream_id : -1,
               buffer.stream->width, buffer.stream->height, buffer.stream->format,
               raw_bpp, raw_stride, raw_vaddr);
        }
      }
    }
    if (replaced && unlock_done_ns == lock_done_ns) {
      unlock_start_ns = write_done_ns;
      unlock_done_ns = write_done_ns;
    }
  } else {
    write_start_ns = monotonic_time_ns();
    replaced = write_i420_to_ycbcr(*scaled_i420,
                                   static_cast<int>(buffer.stream->width),
                                   static_cast<int>(buffer.stream->height), layout);
    write_done_ns = monotonic_time_ns();
    unlock_start_ns = write_done_ns;
    const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
    unlock_done_ns = monotonic_time_ns();
    if (unlock_rc != 0) {
      LOGW("GraphicBuffer::unlock failed rc=%d", unlock_rc);
    }
  }

  g_graphic_buffer_api.dec_strong(graphic_buffer, dec_strong_cookie);

  if (replaced) {
    const uint64_t replace_count =
        g_replaced_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (replace_count <= 10 || (replace_count % 120) == 0) {
      LOGI("Replaced frame #%llu streamId=%d dst=%ux%u fmt=%#x source=%s %dx%d %s surfaceId=%d",
           static_cast<unsigned long long>(replace_count),
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height, buffer.stream->format,
           basename_copy(source_descriptor.sequence ? g_source_state.loaded_path
                                                    : source_descriptor.path)
               .c_str(),
           source_descriptor.width, source_descriptor.height,
           source_pixel_format_name(source_descriptor.format), surface_id);
      const uint64_t total_done_ns = monotonic_time_ns();
      LOGI("Perf replace #%llu ready=%d streamId=%d %ux%u step=%zu get=%.3fms load=%.3fms from=%.3fms lock=%.3fms write=%.3fms unlock=%.3fms total=%.3fms",
           static_cast<unsigned long long>(replace_count), ready_path ? 1 : 0,
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height, layout.chroma_step,
           ns_to_ms(get_done_ns - get_start_ns),
           ns_to_ms(load_done_ns - load_start_ns),
           ns_to_ms(from_done_ns - from_start_ns),
           ns_to_ms(lock_done_ns - lock_start_ns),
           ns_to_ms(write_done_ns - write_start_ns),
           ns_to_ms(unlock_done_ns - unlock_start_ns),
           ns_to_ms(total_done_ns - total_start_ns));
    }
  }

  return replaced;
}


bool load_replacement_i420_for_size(int width, int height,
                                    SourceDescriptor *source_descriptor,
                                    const std::vector<uint8_t> **scaled_i420,
                                    bool *ready_path) {
  if (source_descriptor == nullptr || scaled_i420 == nullptr || ready_path == nullptr) {
    return false;
  }
  *scaled_i420 = nullptr;
  *ready_path = false;
  std::shared_ptr<const awesomecam::ReadyI420Frame> ready_frame =
      awesomecam::GetReadyI420Frame(width, height);
  if (ready_frame != nullptr && !ready_frame->bytes.empty()) {
    *scaled_i420 = &ready_frame->bytes;
    source_descriptor->valid = true;
    source_descriptor->sequence = false;
    source_descriptor->path = "ready://NativePlayback";
    source_descriptor->width = ready_frame->width;
    source_descriptor->height = ready_frame->height;
    source_descriptor->fps = kDefaultSourceFps;
    source_descriptor->format = SourcePixelFormat::kI420;
    *ready_path = true;
    return true;
  }

  std::unique_lock<std::mutex> fallback_process_lock(g_frame_process_mutex);
  return load_latest_scaled_i420_for_dst(width, height, source_descriptor, scaled_i420) &&
         *scaled_i420 != nullptr;
}

bool try_replace_anw_buffer_direct(void *anw_buffer, int width, int height, int format,
                                   const char *where) {
  if (anw_buffer == nullptr || width <= 0 || height <= 0) return false;
  if (format != kHalPixelFormatYcbcr420888 &&
      format != kHalPixelFormatImplementationDefined) {
    return false;
  }
  if (!resolve_graphic_buffer_api()) return false;

  SourceDescriptor source_descriptor;
  const std::vector<uint8_t> *scaled_i420 = nullptr;
  bool ready_path = false;
  if (!load_replacement_i420_for_size(width, height, &source_descriptor,
                                      &scaled_i420, &ready_path) ||
      scaled_i420 == nullptr) {
    return false;
  }

  GraphicBufferSpRet graphic_buffer_ref = g_graphic_buffer_api.from(anw_buffer);
  void *graphic_buffer = graphic_buffer_ref.ptr;
  if (graphic_buffer == nullptr) return false;

  bool replaced = false;
  android_ycbcr layout{};
  const int lock_rc = g_graphic_buffer_api.lock_ycbcr(graphic_buffer,
                                                      kGraphicBufferCpuLockUsage,
                                                      &layout);
  if (lock_rc == 0) {
    replaced = write_i420_to_ycbcr(*scaled_i420, width, height, layout);
    const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
    if (unlock_rc != 0) LOGW("%s: queue unlock failed rc=%d", where, unlock_rc);
  } else {
    replaced = try_write_i420_to_ahardwarebuffer_planes(
        graphic_buffer, *scaled_i420, width, height, where);
    if (!replaced) {
      void *raw_vaddr = nullptr;
      int raw_bpp = 0;
      int raw_stride = 0;
      const int raw_rc = g_graphic_buffer_api.lock(graphic_buffer,
                                                   kGraphicBufferCpuLockUsage,
                                                   &raw_vaddr, &raw_bpp, &raw_stride);
      if (raw_rc == 0 && raw_vaddr != nullptr) {
        replaced = write_i420_to_contiguous_nv(*scaled_i420, width, height, raw_vaddr,
                                               raw_stride, prefer_raw_nv21_output());
        const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
        if (unlock_rc != 0) LOGW("%s: queue raw unlock failed rc=%d", where, unlock_rc);
      }
    }
  }

  g_graphic_buffer_api.dec_strong(graphic_buffer, &graphic_buffer_ref);
  if (replaced) {
    const uint64_t c = g_queue_replaced_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 20 || (c % 120) == 0) {
      LOGI("%s replaced queue frame #%llu dst=%dx%d fmt=%#x source=%s %dx%d %s ready=%d",
           where, static_cast<unsigned long long>(c), width, height, format,
           basename_copy(source_descriptor.sequence ? g_source_state.loaded_path
                                                    : source_descriptor.path).c_str(),
           source_descriptor.width, source_descriptor.height,
           source_pixel_format_name(source_descriptor.format), ready_path ? 1 : 0);
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
    (void)wait_fence_if_valid(anw_release_fence,
                              "queueBufferToConsumer release");
    (void)try_replace_anw_buffer_direct(anw_buffer, width, height, format,
                                        "queueBufferToConsumer");
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
    (void)wait_fence_if_valid(fence_fd, "Surface::hook_queueBuffer fence");
    (void)try_replace_anw_buffer_direct(anw_buffer, width, height, format,
                                        "Surface::hook_queueBuffer");
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
    // createStream hook is disabled on current Android 16 build because the old
    // offset is stale. Learn the target sizes from actual returned buffers.
    register_ready_target_if_supported(stream->width, stream->height, stream->format);

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

  if (output && stream != nullptr) {
    // Camera HAL may return a buffer with an unsignaled acquire fence.  If we
    // CPU-write before that fence signals, HAL/GPU can still overwrite our
    // replacement and the app sees the original frame.  Wait first, then write.
    (void)wait_fence_if_valid(buffer.acquire_fence,
                              "returnBufferCheckedLocked acquire");
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

  const uintptr_t base = find_cameraserver_base();
  if (base == 0) {
    LOGE("failed to locate cameraserver base");
    return;
  }
  LOGI("install_hook: cameraserver base=%p", reinterpret_cast<void *>(base));

  const HookOffsetConfig offset_cfg = resolve_hook_offset_config();

  if (offset_cfg.enable_create_configured_surface) {
    const uintptr_t target = base + offset_cfg.create_configured_surface;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("createConfiguredSurface target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_create_surface_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_create_configured_surface),
                                  &g_orig_create_configured_surface);
    if (g_create_surface_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook createConfiguredSurface failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kCreateConfiguredSurfaceSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(offset_cfg.create_configured_surface),
           g_orig_create_configured_surface);
    }
  } else {
    LOGW("createConfiguredSurface hook disabled; using Camera3Device::createStream/returnBuffer target learning");
  }

  if (offset_cfg.enable_create_stream) {
    const uintptr_t target = base + offset_cfg.create_stream;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("Camera3Device::createStream target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_camera3device_create_stream_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_camera3device_create_stream),
                                  &g_orig_camera3device_create_stream);
    if (g_camera3device_create_stream_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook Camera3Device::createStream failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kCamera3DeviceCreateStreamSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(offset_cfg.create_stream),
           g_orig_camera3device_create_stream);
    }
  } else {
    LOGW("Camera3Device::createStream hook disabled");
  }


  if (kEnableQueueBufferToConsumerHook) {
    const uintptr_t target = base + kQueueBufferToConsumerOffset;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("queueBufferToConsumer target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }
    g_queue_buffer_to_consumer_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_queue_buffer_to_consumer),
                                  &g_orig_queue_buffer_to_consumer);
    if (g_queue_buffer_to_consumer_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook queueBufferToConsumer failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kQueueBufferToConsumerSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(kQueueBufferToConsumerOffset),
           g_orig_queue_buffer_to_consumer);
    }
  }

  if (offset_cfg.enable_return_buffer_checked_locked) {
    const uintptr_t target = base + offset_cfg.return_buffer_checked_locked;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("returnBufferCheckedLocked target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_return_buffer_checked_locked_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_return_buffer_checked_locked),
                                  &g_orig_return_buffer_checked_locked);
    if (g_return_buffer_checked_locked_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook returnBufferCheckedLocked failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kReturnBufferCheckedLockedSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(offset_cfg.return_buffer_checked_locked),
           g_orig_return_buffer_checked_locked);
    }
  } else {
    LOGW("returnBufferCheckedLocked hook disabled");
  }
}

}  // namespace

extern "C" void log_create_configured_surface_result(void *stream_info_ptr,
                                                      uint64_t is_stream_info_valid) {
  const uint64_t count =
      g_surface_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (stream_info_ptr == nullptr) {
    LOGE("SessionConfigurationUtils::createSurfaceFromGbp hit #%llu valid=%llu streamInfo=null",
         static_cast<unsigned long long>(count),
         static_cast<unsigned long long>(is_stream_info_valid));
    return;
  }

  const auto *info = reinterpret_cast<const OutputStreamInfoPrefix *>(stream_info_ptr);
  register_ready_target_if_supported(static_cast<uint32_t>(std::max(0, info->width)),
                                     static_cast<uint32_t>(std::max(0, info->height)),
                                     info->format);
  LOGI("SessionConfigurationUtils::createSurfaceFromGbp hit #%llu valid=%llu width=%d height=%d format=%#x dataspace=%#x",
       static_cast<unsigned long long>(count),
       static_cast<unsigned long long>(is_stream_info_valid), info->width,
       info->height, info->format, info->data_space);
}

extern "C" __attribute__((naked, noinline)) void hook_create_configured_surface() {
#if defined(__aarch64__)
  __asm__ __volatile__(
      "sub sp, sp, #0x90\n"
      "stp x29, x30, [sp, #0x80]\n"
      "stp x19, x20, [sp, #0x30]\n"
      "stp x21, x22, [sp, #0x40]\n"
      "stp x23, x24, [sp, #0x50]\n"
      "stp x25, x26, [sp, #0x60]\n"
      "stp x27, x28, [sp, #0x70]\n"

      "mov x19, x0\n"
      "mov x20, x1\n"
      "mov x21, x2\n"
      "mov x22, x3\n"
      "mov x23, x4\n"
      "mov x24, x5\n"
      "mov x25, x6\n"
      "mov x26, x7\n"
      "mov x27, x8\n"

      "ldr x9, [sp, #0x90]\n"
      "str x9, [sp, #0x00]\n"
      "ldr x9, [sp, #0x98]\n"
      "str x9, [sp, #0x08]\n"
      "ldr x9, [sp, #0xa0]\n"
      "str x9, [sp, #0x10]\n"
      "ldr x9, [sp, #0xa8]\n"
      "str x9, [sp, #0x18]\n"
      "ldr x9, [sp, #0xb0]\n"
      "str x9, [sp, #0x20]\n"

      "adrp x16, :got:g_orig_create_configured_surface\n"
      "ldr x16, [x16, :got_lo12:g_orig_create_configured_surface]\n"
      "ldr x16, [x16]\n"
      "mov x0, x19\n"
      "mov x1, x20\n"
      "mov x2, x21\n"
      "mov x3, x22\n"
      "mov x4, x23\n"
      "mov x5, x24\n"
      "mov x6, x25\n"
      "mov x7, x26\n"
      "mov x8, x27\n"
      "blr x16\n"

      "str x0, [sp, #0x28]\n"
      "mov x0, x19\n"
      "mov x1, x20\n"
      "bl log_create_configured_surface_result\n"
      "ldr x0, [sp, #0x28]\n"

      "ldp x19, x20, [sp, #0x30]\n"
      "ldp x21, x22, [sp, #0x40]\n"
      "ldp x23, x24, [sp, #0x50]\n"
      "ldp x25, x26, [sp, #0x60]\n"
      "ldp x27, x28, [sp, #0x70]\n"
      "ldp x29, x30, [sp, #0x80]\n"
      "add sp, sp, #0x90\n"
      "ret\n");
#else
#error "hook_create_configured_surface is only implemented for aarch64"
#endif
}

namespace {

void AutoStartNativePlaybackIfAvailable() {
  constexpr const char *kDefaultMp4Path = "/data/camera/input.mp4";
  if (awesomecam::IsNativePlaybackRunning()) {
    LOGI("main_hook: native playback already running");
    return;
  }
  if (access(kDefaultMp4Path, R_OK) != 0) {
    LOGW("main_hook: auto native playback skipped path=%s access=%s",
         kDefaultMp4Path, strerror(errno));
    return;
  }
  std::string error;
  if (awesomecam::StartNativePlayback(kDefaultMp4Path, &error)) {
    LOGI("main_hook: auto native playback started path=%s", kDefaultMp4Path);
  } else {
    LOGE("main_hook: auto native playback start failed path=%s error=%s",
         kDefaultMp4Path, error.c_str());
  }
}

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
  AutoStartNativePlaybackIfAvailable();
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
