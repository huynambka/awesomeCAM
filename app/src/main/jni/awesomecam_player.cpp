#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
}

#include "libyuv_runtime.h"
#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_verbose_enabled{0};
std::atomic<int64_t> g_verbose_last_refresh_us{0};

constexpr int32_t kColorFormatYuv420Planar = 19;
constexpr int32_t kColorFormatYuv420PackedPlanar = 20;
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;
constexpr int32_t kColorFormatYuv420PackedSemiPlanar = 39;
constexpr int32_t kColorFormatYuv420Flexible = 0x7f420888;
constexpr int64_t kCodecIoTimeoutUs = 10000;

struct Options {
  std::string input;
  std::string rtmp_url_file;
  std::string pidfile = "/data/camera/awesomecam_player.pid";
  bool loop = true;
  bool auto_variant = true;
  bool live = false;
  int fps_cap = 30;
  bool fps_cap_cli = false;
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
    width = 0;
    height = 0;
    slot_size = 0;
    next_slot = 0;
    next_generation = 0;
  }

  awesomecam::SharedMemoryRingHeader *header() const {
    return reinterpret_cast<awesomecam::SharedMemoryRingHeader *>(addr);
  }
};

struct RingWriteSlot {
  uint8_t *data = nullptr;
  uint32_t slot_index = awesomecam::kSharedMemoryNoSlot;
  uint64_t generation = 0;
  int64_t pts_us = 0;
};

enum class OutputLayoutKind {
  kUnknown,
  kPlanar,
  kSemiPlanar,
};

struct OutputLayout {
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t slice_height = 0;
  int32_t crop_left = 0;
  int32_t crop_top = 0;
  int32_t color_format = 0;
  OutputLayoutKind kind = OutputLayoutKind::kUnknown;
  bool chroma_swapped = false;
};

struct TrackInfo {
  size_t index = 0;
  std::string mime;
  int32_t width = 0;
  int32_t height = 0;
  int32_t frame_rate = 0;
  int64_t duration_us = -1;
  AMediaFormat *format = nullptr;
};

struct RtmpInput {
  std::string url_redacted;
  AVFormatContext *format = nullptr;
  AVBSFContext *bsf = nullptr;
  AVPacket *raw = nullptr;
  AVPacket *filtered = nullptr;
  int video_stream = -1;
  AVRational time_base{1, AV_TIME_BASE};
  std::string mime;
  int32_t width = 0;
  int32_t height = 0;
  int32_t frame_rate = 0;
  int64_t duration_us = -1;
  int64_t last_pts_us = -1;

  ~RtmpInput() { reset(); }

  void reset() {
    if (filtered != nullptr) av_packet_free(&filtered);
    if (raw != nullptr) av_packet_free(&raw);
    if (bsf != nullptr) av_bsf_free(&bsf);
    if (format != nullptr) avformat_close_input(&format);
    video_stream = -1;
  }
};

struct TargetSnapshot {
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t generation = 0;
  int64_t last_seen_ns = 0;
  uint64_t hit_count = 0;
};

struct VariantSpec {
  const char *name;
  int32_t width;
  int32_t height;
  const char *path;
};

constexpr VariantSpec kVariantSpecs[] = {
    {"1440x1080", 1440, 1080, "/data/camera/input_1440x1080.mp4"},
    {"1280x720", 1280, 720, "/data/camera/input_1280x720.mp4"},
    {"1920x1080", 1920, 1080, "/data/camera/input_1920x1080.mp4"},
    {"640x480", 640, 480, "/data/camera/input_640x480.mp4"},
};

enum class DecodeOutcome {
  kFailed,
  kEof,
  kStopped,
  kSwitchInput,
};

void SignalHandler(int) { g_stop.store(true, std::memory_order_release); }

void TuneCurrentThreadPriority(const char *where) {
  const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  const int rc = setpriority(PRIO_PROCESS, tid, -4);
  if (rc == 0) {
    LOGI("MediaCodecPlayer: priority tuned tid=%d nice=-4 where=%s",
         tid, where != nullptr ? where : "main");
  } else if (access("/data/camera/awesomecam_verbose", F_OK) == 0) {
    LOGW("MediaCodecPlayer: priority tune failed tid=%d errno=%d where=%s",
         tid, errno, where != nullptr ? where : "main");
  }
}

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

bool VerboseLoggingEnabled() {
  const int64_t now_us = MonotonicUs();
  int64_t last_us = g_verbose_last_refresh_us.load(std::memory_order_acquire);
  if (last_us == 0 || now_us - last_us >= 1000000LL) {
    if (g_verbose_last_refresh_us.compare_exchange_strong(
            last_us, now_us, std::memory_order_acq_rel, std::memory_order_acquire)) {
      g_verbose_enabled.store(access("/data/camera/awesomecam_verbose", F_OK) == 0 ? 1 : 0,
                              std::memory_order_release);
    }
  }
  return g_verbose_enabled.load(std::memory_order_acquire) != 0;
}

bool ShouldLogCounter(uint64_t count, uint64_t verbose_first = 5,
                      uint64_t rare_every = 120) {
  return (rare_every != 0 && (count % rare_every) == 0) ||
         (VerboseLoggingEnabled() && count <= verbose_first);
}

size_t ChromaWidth(int32_t width) { return static_cast<size_t>((width + 1) / 2); }
size_t ChromaHeight(int32_t height) { return static_cast<size_t>((height + 1) / 2); }

bool StartsWith(const std::string &value, const char *prefix) {
  const size_t len = strlen(prefix);
  return value.size() >= len && value.compare(0, len, prefix) == 0;
}

bool StartsWithIgnoreCase(const std::string &value, const char *prefix) {
  const size_t len = strlen(prefix);
  if (value.size() < len) return false;
  for (size_t i = 0; i < len; ++i) {
    const char a = static_cast<char>(tolower(value[i]));
    const char b = static_cast<char>(tolower(prefix[i]));
    if (a != b) return false;
  }
  return true;
}

std::string AvErrorString(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

std::string ReadSmallTextFile(const std::string &path, size_t max_len = 4096) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  std::string out;
  out.resize(max_len);
  const ssize_t n = read(fd, out.data(), out.size());
  close(fd);
  if (n <= 0) return "";
  out.resize(static_cast<size_t>(n));
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                          out.back() == '\t' || out.back() == ' ')) {
    out.pop_back();
  }
  return out;
}

std::string RedactRtmpUrl(const std::string &url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return "rtmp://<redacted>";
  const std::string scheme = url.substr(0, scheme_end);
  const size_t authority_start = scheme_end + 3;
  const size_t path_start = url.find('/', authority_start);
  std::string authority = path_start == std::string::npos
                              ? url.substr(authority_start)
                              : url.substr(authority_start, path_start - authority_start);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  if (authority.empty()) authority = "<host>";
  std::string app = "<app>";
  if (path_start != std::string::npos && path_start + 1 < url.size()) {
    const size_t next = url.find('/', path_start + 1);
    app = url.substr(path_start + 1, next == std::string::npos
                                         ? std::string::npos
                                         : next - path_start - 1);
    if (app.empty()) app = "<app>";
  }
  return scheme + "://" + authority + "/" + app + "/<redacted>";
}

bool IsRtmpUrl(const std::string &url) {
  return StartsWithIgnoreCase(url, "rtmp://") || StartsWithIgnoreCase(url, "rtmpt://");
}

void PrintUsage() {
  LOGI("MediaCodecPlayer usage: awesomecam_player (--input /data/camera/input_1440x1080.mp4 | --rtmp-url-file /data/camera/awesomecam_rtmp_url --live) --fps-cap 30 --pidfile /data/camera/awesomecam_player.pid");
}

int ClampFpsCap(int value) {
  if (value <= 0) return 0;
  if (value < 5) return 5;
  if (value > 120) return 120;
  return value;
}

int ReadFpsCapFile(int fallback) {
  int fd = open("/data/camera/awesomecam_fps_cap", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fallback;
  char buf[32];
  const ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return fallback;
  buf[n] = '\0';
  char *end = nullptr;
  const long parsed = strtol(buf, &end, 10);
  if (end == buf) return fallback;
  return ClampFpsCap(static_cast<int>(parsed));
}

Options ParseOptions(int argc, char **argv) {
  Options opt{};
  bool input_set = false;
  bool rtmp_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i] ? argv[i] : "";
    auto need_value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        LOGW("MediaCodecPlayer: missing value for %s", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--input") {
      if (const char *v = need_value("--input")) {
        opt.input = v;
        input_set = true;
      }
    } else if (arg == "--rtmp-url-file") {
      if (const char *v = need_value("--rtmp-url-file")) {
        opt.rtmp_url_file = v;
        rtmp_set = true;
      }
    } else if (arg == "--pidfile") {
      if (const char *v = need_value("--pidfile")) opt.pidfile = v;
    } else if (arg == "--once") {
      opt.loop = false;
    } else if (arg == "--loop") {
      opt.loop = true;
    } else if (arg == "--live") {
      opt.live = true;
      opt.loop = false;
      opt.auto_variant = false;
    } else if (arg == "--auto-variant") {
      opt.auto_variant = true;
    } else if (arg == "--fixed-input") {
      opt.auto_variant = false;
    } else if (arg == "--fps-cap") {
      if (const char *v = need_value("--fps-cap")) {
        opt.fps_cap = ClampFpsCap(atoi(v));
        opt.fps_cap_cli = true;
      }
    } else if (arg == "--max-long" || arg == "--max-short" || arg == "--service") {
      // Kept for CLI compatibility with the older FFmpeg player.  Playback now
      // expects already-prescaled input and does not resize while decoding.
      (void)need_value(arg.c_str());
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
    }
  }
  if (input_set == rtmp_set) {
    LOGE("MediaCodecPlayer: exactly one source is required: --input or --rtmp-url-file");
    opt.input.clear();
    opt.rtmp_url_file.clear();
  }
  if (rtmp_set) {
    opt.live = true;
    opt.loop = false;
    opt.auto_variant = false;
  }
  if (!opt.fps_cap_cli) opt.fps_cap = ReadFpsCapFile(opt.fps_cap);
  return opt;
}

void WritePidFile(const std::string &path) {
  if (path.empty()) return;
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    LOGW("MediaCodecPlayer: open pidfile %s failed errno=%d", path.c_str(), errno);
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
    LOGE("MediaCodecPlayer: failed to load binder runtime");
    return false;
  }
  client->api.set_thread_pool_max(1);
  client->api.start_thread_pool();
  LOGI("MediaCodecPlayer: binder threadpool started");
  if (client->client_class == nullptr) {
    client->client_class = client->api.binder_class_define(
        awesomecam::kVideo2CameraDescriptor, ClientOnCreate, ClientOnDestroy, ClientOnTransact);
    if (client->client_class == nullptr) {
      LOGE("MediaCodecPlayer: failed to define binder client class");
      return false;
    }
  }
  client->remote = client->api.wait_for_service(awesomecam::kVideo2CameraServiceName);
  if (client->remote == nullptr) {
    LOGE("MediaCodecPlayer: Video2CameraService unavailable");
    return false;
  }
  if (!client->api.binder_associate_class(client->remote, client->client_class)) {
    LOGE("MediaCodecPlayer: AIBinder_associateClass failed remote=%p", client->remote);
    return false;
  }
  LOGI("MediaCodecPlayer: connected to %s remote=%p", awesomecam::kVideo2CameraServiceName,
       client->remote);
  return true;
}

bool IsReadableRegularFile(const std::string &path) {
  struct stat st {};
  return !path.empty() && access(path.c_str(), R_OK) == 0 &&
         stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

const VariantSpec *FindVariantByName(const std::string &name) {
  for (const VariantSpec &variant : kVariantSpecs) {
    if (name == variant.name) return &variant;
  }
  return nullptr;
}

const VariantSpec *FindVariantBySize(int32_t width, int32_t height) {
  for (const VariantSpec &variant : kVariantSpecs) {
    if (variant.width == width && variant.height == height) return &variant;
  }
  return nullptr;
}

double NormalizedAspect(int32_t width, int32_t height) {
  const int32_t a = std::max<int32_t>(1, std::min(width, height));
  const int32_t b = std::max<int32_t>(1, std::max(width, height));
  return static_cast<double>(a) / static_cast<double>(b);
}

double VariantDistanceScore(const VariantSpec &variant, int32_t target_w,
                            int32_t target_h) {
  const double aspect_delta =
      std::abs(NormalizedAspect(variant.width, variant.height) -
               NormalizedAspect(target_w, target_h));
  const double target_area =
      static_cast<double>(std::max<int32_t>(1, target_w)) *
      static_cast<double>(std::max<int32_t>(1, target_h));
  const double variant_area =
      static_cast<double>(variant.width) * static_cast<double>(variant.height);
  const double area_ratio =
      std::abs(std::log(std::max(variant_area, 1.0) / std::max(target_area, 1.0)));
  return aspect_delta * 8.0 + area_ratio;
}

const VariantSpec *FindNearestReadableVariant(int32_t width, int32_t height,
                                              double *out_score) {
  const VariantSpec *best = nullptr;
  double best_score = 1.0e30;
  for (const VariantSpec &variant : kVariantSpecs) {
    if (!IsReadableRegularFile(variant.path)) continue;
    const double score = VariantDistanceScore(variant, width, height);
    if (best == nullptr || score < best_score) {
      best = &variant;
      best_score = score;
    }
  }
  if (out_score != nullptr) *out_score = best_score;
  return best;
}

std::string ReadVariantOverride() {
  int fd = open("/data/camera/awesomecam_variant", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  char buf[64];
  const ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return "";
  buf[n] = '\0';
  std::string value(buf);
  value.erase(std::remove_if(value.begin(), value.end(), [](char ch) {
                return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
              }),
              value.end());
  return value;
}

bool QueryTargets(BinderClient *client, std::vector<TargetSnapshot> *targets) {
  if (client == nullptr || client->remote == nullptr || targets == nullptr) return false;
  targets->clear();
  AParcel *in = nullptr;
  AParcel *out = nullptr;
  binder_status_t status = client->api.binder_prepare_transaction(client->remote, &in);
  if (status == STATUS_OK) {
    status = client->api.binder_transact(client->remote, awesomecam::kTxnGetTargets,
                                         &in, &out, 0);
  }
  int32_t count = 0;
  if (status == STATUS_OK && out != nullptr) {
    status = client->api.parcel_read_int32(out, &count);
  }
  if (status == STATUS_OK && count > 0 && count < 64) {
    targets->reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
      TargetSnapshot target{};
      int64_t generation = 0;
      int64_t hit_count = 0;
      if (status == STATUS_OK) status = client->api.parcel_read_int32(out, &target.width);
      if (status == STATUS_OK) status = client->api.parcel_read_int32(out, &target.height);
      if (status == STATUS_OK) status = client->api.parcel_read_int32(out, &target.format);
      if (status == STATUS_OK) status = client->api.parcel_read_int64(out, &generation);
      if (status == STATUS_OK) status = client->api.parcel_read_int64(out, &target.last_seen_ns);
      if (status == STATUS_OK) status = client->api.parcel_read_int64(out, &hit_count);
      if (status != STATUS_OK) break;
      target.generation = generation > 0 ? static_cast<uint64_t>(generation) : 0;
      target.hit_count = hit_count > 0 ? static_cast<uint64_t>(hit_count) : 0;
      targets->push_back(target);
    }
  }
  if (in != nullptr) client->api.parcel_delete(in);
  if (out != nullptr) client->api.parcel_delete(out);
  return status == STATUS_OK;
}

std::string ResolveAutoVariantInput(BinderClient *client, const std::string &fallback,
                                    const char *reason) {
  constexpr int64_t kFreshTargetNs = 1500000000LL;
  const std::string override_name = ReadVariantOverride();
  if (!override_name.empty()) {
    const VariantSpec *variant = FindVariantByName(override_name);
    if (variant != nullptr && IsReadableRegularFile(variant->path)) {
      if (fallback != variant->path) {
        LOGI("MediaCodecPlayer: selected override variant %s path=%s reason=%s",
             variant->name, variant->path, reason != nullptr ? reason : "auto");
      }
      return variant->path;
    }
    if (variant == nullptr) {
      LOGW("MediaCodecPlayer: invalid awesomecam_variant '%s'; using auto/default",
           override_name.c_str());
    } else {
      LOGW("MediaCodecPlayer: override variant %s missing path=%s; using auto/default",
           variant->name, variant->path);
    }
  }

  std::vector<TargetSnapshot> targets;
  if (QueryTargets(client, &targets)) {
    static std::string last_unmatched_target;
    static std::string last_nearest_target;
    static std::string last_stale_target;
    const int64_t now_ns = MonotonicUs() * 1000LL;
    const VariantSpec *nearest_variant = nullptr;
    TargetSnapshot nearest_target{};
    double nearest_score = 1.0e30;
    for (const TargetSnapshot &target : targets) {
      const int64_t age_ns = target.last_seen_ns > 0 ? now_ns - target.last_seen_ns : 0;
      if (target.last_seen_ns > 0 && age_ns > kFreshTargetNs) {
        char key[96];
        snprintf(key, sizeof(key), "%dx%d age=%.1fs", target.width, target.height,
                 static_cast<double>(age_ns) / 1000000000.0);
        if (last_stale_target != key) {
          LOGI("MediaCodecPlayer: ignoring stale camera target %s; using %s",
               key, fallback.c_str());
          last_stale_target = key;
        }
        continue;
      }
      const VariantSpec *variant = FindVariantBySize(target.width, target.height);
      if (variant == nullptr) {
        char key[64];
        snprintf(key, sizeof(key), "%dx%d", target.width, target.height);
        if (last_unmatched_target != key) {
          LOGI("MediaCodecPlayer: target %s has no exact mp4 variant; searching nearest prescaled input",
               key);
          last_unmatched_target = key;
        }
        double score = 0.0;
        const VariantSpec *near = FindNearestReadableVariant(target.width, target.height,
                                                             &score);
        if (near != nullptr && score < nearest_score) {
          nearest_variant = near;
          nearest_target = target;
          nearest_score = score;
        }
        continue;
      }
      if (!IsReadableRegularFile(variant->path)) {
        LOGW("MediaCodecPlayer: target %dx%d has variant %s but file missing path=%s",
             target.width, target.height, variant->name, variant->path);
        continue;
      }
      if (fallback != variant->path) {
        LOGI("MediaCodecPlayer: selected target variant %s path=%s hits=%llu reason=%s",
             variant->name, variant->path,
             static_cast<unsigned long long>(target.hit_count),
             reason != nullptr ? reason : "auto");
      }
      return variant->path;
    }
    if (nearest_variant != nullptr) {
      char key[128];
      snprintf(key, sizeof(key), "%dx%d->%s score=%.3f", nearest_target.width,
               nearest_target.height, nearest_variant->name, nearest_score);
      if (last_nearest_target != key || fallback != nearest_variant->path) {
        LOGI("MediaCodecPlayer: selected nearest target variant %s path=%s for target=%dx%d hits=%llu score=%.3f reason=%s; ReadyFrameCache scales final output",
             nearest_variant->name, nearest_variant->path, nearest_target.width,
             nearest_target.height,
             static_cast<unsigned long long>(nearest_target.hit_count), nearest_score,
             reason != nullptr ? reason : "auto");
        last_nearest_target = key;
      }
      return nearest_variant->path;
    }
    static bool logged_no_active_fresh = false;
    if (!logged_no_active_fresh && targets.empty()) {
      LOGI("MediaCodecPlayer: no active camera target yet; using default input %s",
           fallback.c_str());
      logged_no_active_fresh = true;
    }
  } else {
    static bool logged_query_failed = false;
    if (!logged_query_failed) {
      LOGW("MediaCodecPlayer: target query failed; using default input %s",
           fallback.c_str());
      logged_query_failed = true;
    }
  }

  return fallback;
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
    LOGE("MediaCodecPlayer: register source ring failed step=%s status=%d ack=%d",
         failed_step, status, ack);
    return false;
  }
  LOGI("MediaCodecPlayer: source ring registered %dx%d slot=%u region=%zu",
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

  ring->fd = ASharedMemory_create("awesomecam_mediacodec_source", region_size);
  if (ring->fd < 0) {
    LOGE("MediaCodecPlayer: ASharedMemory_create failed errno=%d", errno);
    return false;
  }
  ring->addr = mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
  if (ring->addr == MAP_FAILED) {
    LOGE("MediaCodecPlayer: mmap source ring failed errno=%d", errno);
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

bool BeginWriteFrame(SourceRing *ring, int64_t pts_us, RingWriteSlot *write) {
  if (ring == nullptr || write == nullptr || ring->addr == nullptr ||
      ring->slot_size == 0) {
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
  write->data = static_cast<uint8_t *>(ring->addr) + slot.data_offset;
  write->slot_index = slot_index;
  write->generation = generation;
  write->pts_us = pts_us;
  return true;
}

void FinishWriteFrame(SourceRing *ring, const RingWriteSlot &write) {
  if (ring == nullptr || ring->addr == nullptr ||
      write.slot_index >= awesomecam::kSharedMemoryRingSlotCount) {
    return;
  }
  auto *header = ring->header();
  awesomecam::SharedMemoryRingSlot &slot = header->slots[write.slot_index];
  AtomicStoreRelease(&slot.data_size, ring->slot_size);
  AtomicStoreRelease(&slot.pts_us, write.pts_us);
  AtomicStoreRelease(&slot.end_generation, write.generation);
  AtomicStoreRelease(&header->latest_slot, write.slot_index);
  AtomicStoreRelease(&header->latest_generation, write.generation);
}

bool WriteFrame(SourceRing *ring, const uint8_t *i420, size_t size, int64_t pts_us) {
  if (ring == nullptr || ring->addr == nullptr || i420 == nullptr || size < ring->slot_size) {
    return false;
  }
  RingWriteSlot write{};
  if (!BeginWriteFrame(ring, pts_us, &write) || write.data == nullptr) return false;
  memcpy(write.data, i420, ring->slot_size);
  FinishWriteFrame(ring, write);
  return true;
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
  if (sleep_us < -250000) {
    *base_wall_us = now_us;
    *base_pts_us = pts_us;
    return;
  }
  if (sleep_us > 1000 && !g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
}

bool ShouldDropForFpsCap(const Options &opt, int64_t pts_us, bool clock_started,
                         int64_t base_wall_us, int64_t base_pts_us,
                         int64_t last_published_pts_us) {
  (void)clock_started;
  (void)base_wall_us;
  (void)base_pts_us;
  if (opt.fps_cap <= 0 || pts_us < 0) return false;
  const int64_t min_delta_us = std::max<int64_t>(1, 1000000LL / opt.fps_cap);
  if (last_published_pts_us >= 0 &&
      pts_us - last_published_pts_us < (min_delta_us * 9) / 10) {
    return true;
  }
  return false;
}

bool PreferMediaCodecNv21() {
  return access("/data/camera/awesomecam_mediacodec_nv21", F_OK) == 0;
}

bool ReadInt32(AMediaFormat *fmt, const char *key, int32_t fallback, int32_t *out) {
  if (out == nullptr) return false;
  int32_t value = fallback;
  const bool present = fmt != nullptr && AMediaFormat_getInt32(fmt, key, &value);
  *out = value;
  return present;
}

bool ReadCrop(AMediaFormat *fmt, int32_t width, int32_t height, int32_t *left,
              int32_t *top, int32_t *right, int32_t *bottom) {
  if (left == nullptr || top == nullptr || right == nullptr || bottom == nullptr) return false;
  *left = 0;
  *top = 0;
  *right = std::max<int32_t>(0, width - 1);
  *bottom = std::max<int32_t>(0, height - 1);

  int32_t l = 0;
  int32_t t = 0;
  int32_t r = width - 1;
  int32_t b = height - 1;
  bool found = false;
  if (fmt != nullptr && AMediaFormat_getRect(fmt, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b)) {
    found = true;
  } else if (fmt != nullptr &&
             AMediaFormat_getInt32(fmt, "crop-left", &l) &&
             AMediaFormat_getInt32(fmt, "crop-top", &t) &&
             AMediaFormat_getInt32(fmt, "crop-right", &r) &&
             AMediaFormat_getInt32(fmt, "crop-bottom", &b)) {
    found = true;
  }
  if (found && l >= 0 && t >= 0 && r >= l && b >= t) {
    *left = l;
    *top = t;
    *right = r;
    *bottom = b;
  }
  return found;
}

const char *LayoutKindName(OutputLayoutKind kind) {
  switch (kind) {
    case OutputLayoutKind::kPlanar:
      return "planar";
    case OutputLayoutKind::kSemiPlanar:
      return "semiplanar";
    default:
      return "unknown";
  }
}

bool UpdateOutputLayout(AMediaFormat *fmt, int32_t fallback_w, int32_t fallback_h,
                        OutputLayout *layout, bool log_errors = true) {
  if (fmt == nullptr || layout == nullptr) return false;
  int32_t coded_w = fallback_w;
  int32_t coded_h = fallback_h;
  ReadInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, fallback_w, &coded_w);
  ReadInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, fallback_h, &coded_h);
  if (coded_w <= 0 || coded_h <= 0) {
    if (log_errors) {
      LOGE("MediaCodecPlayer: output format failed invalid coded size %dx%d format=%s",
           coded_w, coded_h, AMediaFormat_toString(fmt));
    }
    return false;
  }

  int32_t color_format = 0;
  if (!ReadInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0, &color_format) || color_format == 0) {
    if (log_errors) {
      LOGE("MediaCodecPlayer: output format failed missing color-format format=%s",
           AMediaFormat_toString(fmt));
    }
    return false;
  }

  OutputLayoutKind kind = OutputLayoutKind::kUnknown;
  switch (color_format) {
    case kColorFormatYuv420Planar:
    case kColorFormatYuv420PackedPlanar:
      kind = OutputLayoutKind::kPlanar;
      break;
    case kColorFormatYuv420SemiPlanar:
    case kColorFormatYuv420PackedSemiPlanar:
    case kColorFormatYuv420Flexible:
      kind = OutputLayoutKind::kSemiPlanar;
      break;
    default:
      if (log_errors) {
        LOGE("MediaCodecPlayer: unsupported color-format failed color=%#x format=%s",
             color_format, AMediaFormat_toString(fmt));
      }
      return false;
  }

  int32_t stride = coded_w;
  int32_t slice_height = coded_h;
  ReadInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, coded_w, &stride);
  ReadInt32(fmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, coded_h, &slice_height);
  if (stride <= 0) stride = coded_w;
  if (slice_height <= 0) slice_height = coded_h;

  int32_t crop_left = 0;
  int32_t crop_top = 0;
  int32_t crop_right = coded_w - 1;
  int32_t crop_bottom = coded_h - 1;
  ReadCrop(fmt, coded_w, coded_h, &crop_left, &crop_top, &crop_right, &crop_bottom);
  int32_t visible_w = crop_right - crop_left + 1;
  int32_t visible_h = crop_bottom - crop_top + 1;
  if (visible_w <= 0 || visible_h <= 0) {
    visible_w = coded_w;
    visible_h = coded_h;
    crop_left = 0;
    crop_top = 0;
  }
  if ((visible_w & 1) != 0 && visible_w > 1) --visible_w;
  if ((visible_h & 1) != 0 && visible_h > 1) --visible_h;
  crop_left &= ~1;
  crop_top &= ~1;

  if (stride < crop_left + visible_w || slice_height < crop_top + visible_h) {
    if (log_errors) {
      LOGE("MediaCodecPlayer: output layout failed stride/slice too small stride=%d slice=%d crop=%d,%d %dx%d format=%s",
           stride, slice_height, crop_left, crop_top, visible_w, visible_h,
           AMediaFormat_toString(fmt));
    }
    return false;
  }

  layout->width = visible_w;
  layout->height = visible_h;
  layout->stride = stride;
  layout->slice_height = slice_height;
  layout->crop_left = crop_left;
  layout->crop_top = crop_top;
  layout->color_format = color_format;
  layout->kind = kind;
  layout->chroma_swapped = PreferMediaCodecNv21();
  LOGI("MediaCodecPlayer: output layout kind=%s color=%#x visible=%dx%d coded=%dx%d stride=%d slice=%d crop=%d,%d nv21Override=%d",
       LayoutKindName(kind), color_format, layout->width, layout->height, coded_w, coded_h,
       layout->stride, layout->slice_height, layout->crop_left, layout->crop_top,
       layout->chroma_swapped ? 1 : 0);
  return true;
}

bool I420PlanesForBuffer(uint8_t *dst, size_t dst_size, int32_t width, int32_t height,
                         uint8_t **dst_y, uint8_t **dst_u, uint8_t **dst_v) {
  if (dst == nullptr || dst_y == nullptr || dst_u == nullptr || dst_v == nullptr ||
      width <= 0 || height <= 0) {
    return false;
  }
  const size_t needed = awesomecam::I420FrameSize(width, height);
  if (needed == 0 || dst_size < needed) return false;
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t c_size = ChromaWidth(width) * ChromaHeight(height);
  *dst_y = dst;
  *dst_u = dst + y_size;
  *dst_v = dst + y_size + c_size;
  return true;
}

bool ConvertPlanarToI420Buffer(const uint8_t *src, size_t src_size,
                               const OutputLayout &layout,
                               uint8_t *dst, size_t dst_size) {
  if (src == nullptr || dst == nullptr || layout.width <= 0 || layout.height <= 0) return false;
  const size_t y_plane_size = static_cast<size_t>(layout.stride) * layout.slice_height;
  const int32_t chroma_stride = std::max<int32_t>(1, layout.stride / 2);
  const int32_t chroma_slice_height = std::max<int32_t>(1, layout.slice_height / 2);
  const size_t chroma_plane_size = static_cast<size_t>(chroma_stride) * chroma_slice_height;
  if (src_size < y_plane_size + 2 * chroma_plane_size) return false;

  const size_t cw = ChromaWidth(layout.width);
  const size_t ch = ChromaHeight(layout.height);
  uint8_t *dst_y = nullptr;
  uint8_t *dst_u = nullptr;
  uint8_t *dst_v = nullptr;
  if (!I420PlanesForBuffer(dst, dst_size, layout.width, layout.height,
                           &dst_y, &dst_u, &dst_v)) {
    return false;
  }

  for (int32_t row = 0; row < layout.height; ++row) {
    const size_t src_off = static_cast<size_t>(layout.crop_top + row) * layout.stride +
                           layout.crop_left;
    if (src_off + layout.width > src_size) return false;
    memcpy(dst_y + static_cast<size_t>(row) * layout.width, src + src_off, layout.width);
  }

  const uint8_t *src_u_base = src + y_plane_size;
  const uint8_t *src_v_base = src_u_base + chroma_plane_size;
  const int32_t chroma_crop_left = layout.crop_left / 2;
  const int32_t chroma_crop_top = layout.crop_top / 2;
  for (size_t row = 0; row < ch; ++row) {
    const size_t src_off = static_cast<size_t>(chroma_crop_top) * chroma_stride +
                           row * chroma_stride + chroma_crop_left;
    if (src_off + cw > chroma_plane_size) return false;
    memcpy(dst_u + row * cw, src_u_base + src_off, cw);
    memcpy(dst_v + row * cw, src_v_base + src_off, cw);
  }
  return true;
}

bool ConvertSemiPlanarToI420Buffer(const uint8_t *src, size_t src_size,
                                   const OutputLayout &layout,
                                   uint8_t *dst, size_t dst_size) {
  if (src == nullptr || dst == nullptr || layout.width <= 0 || layout.height <= 0) return false;
  const size_t y_plane_size = static_cast<size_t>(layout.stride) * layout.slice_height;
  if (src_size < y_plane_size) return false;

  const size_t cw = ChromaWidth(layout.width);
  const size_t ch = ChromaHeight(layout.height);
  uint8_t *dst_y = nullptr;
  uint8_t *dst_u = nullptr;
  uint8_t *dst_v = nullptr;
  if (!I420PlanesForBuffer(dst, dst_size, layout.width, layout.height,
                           &dst_y, &dst_u, &dst_v)) {
    return false;
  }

  const uint8_t *src_y = src + static_cast<size_t>(layout.crop_top) * layout.stride +
                         layout.crop_left;
  const uint8_t *src_uv = src + y_plane_size +
                          static_cast<size_t>(layout.crop_top / 2) * layout.stride +
                          layout.crop_left;
  const size_t uv_size = src_size - y_plane_size;
  const size_t uv_first_off =
      static_cast<size_t>(layout.crop_top / 2) * layout.stride + layout.crop_left;
  if (uv_first_off + (ch > 0 ? (ch - 1) * static_cast<size_t>(layout.stride) : 0) +
          cw * 2 >
      uv_size) {
    return false;
  }

  const bool libyuv_ok = layout.chroma_swapped
      ? awesomecam::LibYuvNV21ToI420(src_y, layout.stride, src_uv, layout.stride,
                                     dst_y, layout.width, dst_u, static_cast<int>(cw),
                                     dst_v, static_cast<int>(cw),
                                     layout.width, layout.height)
      : awesomecam::LibYuvNV12ToI420(src_y, layout.stride, src_uv, layout.stride,
                                     dst_y, layout.width, dst_u, static_cast<int>(cw),
                                     dst_v, static_cast<int>(cw),
                                     layout.width, layout.height);
  if (libyuv_ok) return true;

  for (int32_t row = 0; row < layout.height; ++row) {
    const size_t src_off = static_cast<size_t>(layout.crop_top + row) * layout.stride +
                           layout.crop_left;
    if (src_off + layout.width > src_size) return false;
    memcpy(dst_y + static_cast<size_t>(row) * layout.width, src + src_off, layout.width);
  }

  const int32_t chroma_crop_top = layout.crop_top / 2;
  const int32_t chroma_crop_left_bytes = layout.crop_left;
  for (size_t row = 0; row < ch; ++row) {
    const size_t row_off = static_cast<size_t>(chroma_crop_top) * layout.stride +
                           row * layout.stride + chroma_crop_left_bytes;
    if (row_off + cw * 2 > uv_size) return false;
    const uint8_t *line = src_uv + row_off;
    for (size_t col = 0; col < cw; ++col) {
      if (layout.chroma_swapped) {
        dst_v[row * cw + col] = line[col * 2 + 0];
        dst_u[row * cw + col] = line[col * 2 + 1];
      } else {
        dst_u[row * cw + col] = line[col * 2 + 0];
        dst_v[row * cw + col] = line[col * 2 + 1];
      }
    }
  }
  return true;
}

bool ConvertOutputToI420Buffer(const uint8_t *src, size_t src_size,
                               const OutputLayout &layout,
                               uint8_t *dst, size_t dst_size) {
  switch (layout.kind) {
    case OutputLayoutKind::kPlanar:
      return ConvertPlanarToI420Buffer(src, src_size, layout, dst, dst_size);
    case OutputLayoutKind::kSemiPlanar:
      return ConvertSemiPlanarToI420Buffer(src, src_size, layout, dst, dst_size);
    default:
      return false;
  }
}

bool ConvertOutputToI420(const uint8_t *src, size_t src_size, const OutputLayout &layout,
                         std::vector<uint8_t> *dst) {
  if (dst == nullptr) return false;
  dst->resize(awesomecam::I420FrameSize(layout.width, layout.height));
  return ConvertOutputToI420Buffer(src, src_size, layout, dst->data(), dst->size());
}

bool OpenExtractor(const std::string &path, AMediaExtractor **out_extractor, int *out_fd) {
  if (out_extractor == nullptr || out_fd == nullptr) return false;
  *out_extractor = nullptr;
  *out_fd = -1;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    LOGE("MediaCodecPlayer: open input failed path=%s errno=%d (%s)",
         path.c_str(), errno, strerror(errno));
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    LOGE("MediaCodecPlayer: stat input failed path=%s errno=%d size=%lld",
         path.c_str(), errno, static_cast<long long>(st.st_size));
    close(fd);
    return false;
  }

  AMediaExtractor *extractor = AMediaExtractor_new();
  if (extractor == nullptr) {
    LOGE("MediaCodecPlayer: AMediaExtractor_new failed");
    close(fd);
    return false;
  }
  const media_status_t status = AMediaExtractor_setDataSourceFd(
      extractor, fd, 0, static_cast<off64_t>(st.st_size));
  if (status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: AMediaExtractor_setDataSourceFd failed status=%d path=%s",
         status, path.c_str());
    AMediaExtractor_delete(extractor);
    close(fd);
    return false;
  }
  *out_extractor = extractor;
  *out_fd = fd;
  return true;
}

bool FindVideoTrack(AMediaExtractor *extractor, TrackInfo *track) {
  if (extractor == nullptr || track == nullptr) return false;
  const size_t count = AMediaExtractor_getTrackCount(extractor);
  for (size_t i = 0; i < count; ++i) {
    AMediaFormat *fmt = AMediaExtractor_getTrackFormat(extractor, i);
    if (fmt == nullptr) continue;
    const char *mime_c = nullptr;
    if (!AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime_c) || mime_c == nullptr) {
      AMediaFormat_delete(fmt);
      continue;
    }
    std::string mime = mime_c;
    if (!StartsWith(mime, "video/")) {
      AMediaFormat_delete(fmt);
      continue;
    }
    int32_t width = 0;
    int32_t height = 0;
    if (!AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &width) ||
        !AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &height) ||
        width <= 0 || height <= 0) {
      LOGE("MediaCodecPlayer: video track format failed missing size format=%s",
           AMediaFormat_toString(fmt));
      AMediaFormat_delete(fmt);
      return false;
    }
    int32_t frame_rate = 0;
    int64_t duration_us = -1;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, &frame_rate);
    AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &duration_us);
    track->index = i;
    track->mime = mime;
    track->width = width;
    track->height = height;
    track->frame_rate = frame_rate;
    track->duration_us = duration_us;
    track->format = fmt;
    return true;
  }
  LOGE("MediaCodecPlayer: no video stream found tracks=%zu", count);
  return false;
}

void SetH264CodecSpecificData(AMediaFormat *format, const uint8_t *data,
                              size_t size);

bool OpenRtmpInput(const std::string &url_file, RtmpInput *rtmp,
                   AMediaFormat **out_format) {
  if (rtmp == nullptr || out_format == nullptr) return false;
  *out_format = nullptr;
  const std::string url = ReadSmallTextFile(url_file);
  if (url.empty()) {
    LOGE("MediaCodecPlayer: RTMP URL file missing/empty path=%s", url_file.c_str());
    return false;
  }
  if (!IsRtmpUrl(url)) {
    LOGE("MediaCodecPlayer: RTMP URL must start with rtmp:// or rtmpt:// source=%s",
         RedactRtmpUrl(url).c_str());
    return false;
  }
  rtmp->url_redacted = RedactRtmpUrl(url);

  avformat_network_init();
  av_log_set_level(AV_LOG_QUIET);
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "rw_timeout", "5000000", 0);
  // Do not set RTMP protocol option "timeout" here: for FFmpeg RTMP it is a
  // listen/accept timeout and implies server/listen mode.  In adb reverse
  // setups that makes avformat try to bind 127.0.0.1:1935 and fail with
  // EADDRINUSE instead of connecting to the forwarded host server.
  av_dict_set(&opts, "rtmp_live", "live", 0);
  av_dict_set(&opts, "fflags", "nobuffer", 0);
  av_dict_set(&opts, "flags", "low_delay", 0);

  AVFormatContext *fmt = nullptr;
  int rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    LOGE("MediaCodecPlayer: RTMP open failed source=%s rc=%d (%s)",
         rtmp->url_redacted.c_str(), rc, AvErrorString(rc).c_str());
    return false;
  }
  rtmp->format = fmt;

  rc = avformat_find_stream_info(fmt, nullptr);
  if (rc < 0) {
    LOGE("MediaCodecPlayer: RTMP stream info failed source=%s rc=%d (%s)",
         rtmp->url_redacted.c_str(), rc, AvErrorString(rc).c_str());
    return false;
  }

  int video_stream = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream *stream = fmt->streams[i];
    if (stream != nullptr && stream->codecpar != nullptr &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream = static_cast<int>(i);
      break;
    }
  }
  if (video_stream < 0) {
    LOGE("MediaCodecPlayer: RTMP no video stream source=%s",
         rtmp->url_redacted.c_str());
    return false;
  }

  AVStream *stream = fmt->streams[video_stream];
  AVCodecParameters *par = stream->codecpar;
  if (par->codec_id != AV_CODEC_ID_H264) {
    LOGE("MediaCodecPlayer: RTMP unsupported codec id=%d source=%s",
         static_cast<int>(par->codec_id), rtmp->url_redacted.c_str());
    return false;
  }
  if (par->width <= 0 || par->height <= 0) {
    LOGE("MediaCodecPlayer: RTMP missing video size source=%s",
         rtmp->url_redacted.c_str());
    return false;
  }

  const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
  if (filter == nullptr) {
    LOGE("MediaCodecPlayer: RTMP h264_mp4toannexb bitstream filter unavailable");
    return false;
  }
  AVBSFContext *bsf = nullptr;
  rc = av_bsf_alloc(filter, &bsf);
  if (rc < 0 || bsf == nullptr) {
    LOGE("MediaCodecPlayer: RTMP bsf alloc failed rc=%d (%s)",
         rc, AvErrorString(rc).c_str());
    return false;
  }
  rtmp->bsf = bsf;
  rc = avcodec_parameters_copy(bsf->par_in, par);
  if (rc < 0) {
    LOGE("MediaCodecPlayer: RTMP bsf parameters copy failed rc=%d (%s)",
         rc, AvErrorString(rc).c_str());
    return false;
  }
  bsf->time_base_in = stream->time_base;
  rc = av_bsf_init(bsf);
  if (rc < 0) {
    LOGE("MediaCodecPlayer: RTMP bsf init failed rc=%d (%s)",
         rc, AvErrorString(rc).c_str());
    return false;
  }

  rtmp->raw = av_packet_alloc();
  rtmp->filtered = av_packet_alloc();
  if (rtmp->raw == nullptr || rtmp->filtered == nullptr) {
    LOGE("MediaCodecPlayer: RTMP packet allocation failed");
    return false;
  }
  rtmp->video_stream = video_stream;
  rtmp->time_base = stream->time_base;
  rtmp->mime = "video/avc";
  rtmp->width = par->width;
  rtmp->height = par->height;
  rtmp->duration_us = stream->duration > 0
                          ? av_rescale_q(stream->duration, stream->time_base,
                                         AVRational{1, AV_TIME_BASE})
                          : -1;
  const AVRational fps = av_guess_frame_rate(fmt, stream, nullptr);
  if (fps.num > 0 && fps.den > 0) {
    rtmp->frame_rate = static_cast<int32_t>(std::lround(av_q2d(fps)));
  }

  AMediaFormat *format = AMediaFormat_new();
  if (format == nullptr) {
    LOGE("MediaCodecPlayer: RTMP AMediaFormat_new failed");
    return false;
  }
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, rtmp->mime.c_str());
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, rtmp->width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, rtmp->height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 * 1024 * 1024);
  if (rtmp->frame_rate > 0) {
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, rtmp->frame_rate);
  }
  if (bsf->par_out != nullptr && bsf->par_out->extradata != nullptr &&
      bsf->par_out->extradata_size > 0) {
    SetH264CodecSpecificData(format, bsf->par_out->extradata,
                             static_cast<size_t>(bsf->par_out->extradata_size));
  }
  *out_format = format;

  LOGI("RTMPDemuxer: opened stream codec=h264 mime=video/avc src=%dx%d fps=%d source=%s",
       rtmp->width, rtmp->height, rtmp->frame_rate, rtmp->url_redacted.c_str());
  return true;
}

int ReadFilteredRtmpPacket(RtmpInput *rtmp) {
  if (rtmp == nullptr || rtmp->format == nullptr || rtmp->bsf == nullptr ||
      rtmp->raw == nullptr || rtmp->filtered == nullptr) {
    return AVERROR(EINVAL);
  }
  av_packet_unref(rtmp->filtered);
  while (!g_stop.load(std::memory_order_acquire)) {
    int rc = av_bsf_receive_packet(rtmp->bsf, rtmp->filtered);
    if (rc == 0) return 0;
    if (rc != AVERROR(EAGAIN)) return rc;

    while (!g_stop.load(std::memory_order_acquire)) {
      av_packet_unref(rtmp->raw);
      rc = av_read_frame(rtmp->format, rtmp->raw);
      if (rc < 0) return rc;
      if (rtmp->raw->stream_index != rtmp->video_stream) {
        av_packet_unref(rtmp->raw);
        continue;
      }
      rc = av_bsf_send_packet(rtmp->bsf, rtmp->raw);
      av_packet_unref(rtmp->raw);
      if (rc < 0) return rc;
      break;
    }
  }
  return AVERROR_EXIT;
}

int64_t RtmpPacketPtsUs(RtmpInput *rtmp, const AVPacket *packet) {
  int64_t pts = packet != nullptr ? packet->pts : AV_NOPTS_VALUE;
  if (pts == AV_NOPTS_VALUE && packet != nullptr) pts = packet->dts;
  int64_t pts_us = 0;
  if (pts != AV_NOPTS_VALUE) {
    pts_us = av_rescale_q(pts, rtmp->time_base, AVRational{1, AV_TIME_BASE});
  } else if (rtmp->last_pts_us >= 0) {
    pts_us = rtmp->last_pts_us + 33333;
  }
  if (rtmp->last_pts_us >= 0 && pts_us <= rtmp->last_pts_us) {
    pts_us = rtmp->last_pts_us + 1;
  }
  rtmp->last_pts_us = pts_us;
  return pts_us;
}

const uint8_t *FindStartCode(const uint8_t *begin, const uint8_t *end,
                             size_t *code_size) {
  for (const uint8_t *p = begin; p + 3 <= end; ++p) {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
      if (code_size != nullptr) *code_size = 3;
      return p;
    }
    if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
      if (code_size != nullptr) *code_size = 4;
      return p;
    }
  }
  return nullptr;
}

void SetH264CodecSpecificData(AMediaFormat *format, const uint8_t *data,
                              size_t size) {
  if (format == nullptr || data == nullptr || size == 0) return;
  const uint8_t *begin = data;
  const uint8_t *end = data + size;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  size_t code_size = 0;
  const uint8_t *nal = FindStartCode(begin, end, &code_size);
  while (nal != nullptr) {
    size_t next_code_size = 0;
    const uint8_t *payload = nal + code_size;
    const uint8_t *next = FindStartCode(payload, end, &next_code_size);
    const uint8_t *nal_end = next != nullptr ? next : end;
    if (payload < nal_end) {
      const uint8_t nal_type = payload[0] & 0x1f;
      if (nal_type == 7 && sps.empty()) sps.assign(nal, nal_end);
      if (nal_type == 8 && pps.empty()) pps.assign(nal, nal_end);
    }
    nal = next;
    code_size = next_code_size;
  }
  if (!sps.empty()) {
    AMediaFormat_setBuffer(format, "csd-0", sps.data(), sps.size());
  }
  if (!pps.empty()) {
    AMediaFormat_setBuffer(format, "csd-1", pps.data(), pps.size());
  }
  if (sps.empty() && pps.empty()) {
    AMediaFormat_setBuffer(format, "csd-0", data, size);
  }
}

bool QueueExtractorInput(AMediaCodec *codec, AMediaExtractor *extractor, bool *input_eos) {
  static std::atomic<uint64_t> queued_input_count{0};
  if (codec == nullptr || extractor == nullptr || input_eos == nullptr || *input_eos) {
    return true;
  }
  const ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, kCodecIoTimeoutUs);
  if (input_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
  if (input_index < 0) {
    LOGE("MediaCodecPlayer: dequeueInputBuffer failed rc=%zd", input_index);
    return false;
  }
  size_t capacity = 0;
  uint8_t *buffer = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(input_index), &capacity);
  if (buffer == nullptr || capacity == 0) {
    LOGE("MediaCodecPlayer: getInputBuffer failed idx=%zd capacity=%zu", input_index, capacity);
    return false;
  }

  const ssize_t sample_size = AMediaExtractor_readSampleData(extractor, buffer, capacity);
  if (sample_size < 0) {
    const media_status_t status = AMediaCodec_queueInputBuffer(
        codec, static_cast<size_t>(input_index), 0, 0, 0,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (status != AMEDIA_OK) {
      LOGE("MediaCodecPlayer: queue input EOS failed status=%d", status);
      return false;
    }
    *input_eos = true;
    const uint64_t count = queued_input_count.fetch_add(1, std::memory_order_relaxed) + 1;
    LOGI("MediaCodecPlayer: queued input EOS #%llu",
         static_cast<unsigned long long>(count));
    return true;
  }
  const int64_t pts_us = AMediaExtractor_getSampleTime(extractor);
  const media_status_t status = AMediaCodec_queueInputBuffer(
      codec, static_cast<size_t>(input_index), 0, static_cast<size_t>(sample_size),
      pts_us >= 0 ? static_cast<uint64_t>(pts_us) : 0u, 0);
  if (status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: queueInputBuffer failed status=%d size=%zd pts=%lld",
         status, sample_size, static_cast<long long>(pts_us));
    return false;
  }
  const uint64_t count = queued_input_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ShouldLogCounter(count, 5, 120)) {
    LOGI("MediaCodecPlayer: queued input #%llu idx=%zd size=%zd cap=%zu pts=%lld",
         static_cast<unsigned long long>(count), input_index, sample_size, capacity,
         static_cast<long long>(pts_us));
  }
  (void)AMediaExtractor_advance(extractor);
  return true;
}

bool QueueRtmpInput(AMediaCodec *codec, RtmpInput *rtmp) {
  static std::atomic<uint64_t> queued_rtmp_count{0};
  if (codec == nullptr || rtmp == nullptr) return false;
  const ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, kCodecIoTimeoutUs);
  if (input_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
  if (input_index < 0) {
    LOGE("MediaCodecPlayer: RTMP dequeueInputBuffer failed rc=%zd", input_index);
    return false;
  }
  size_t capacity = 0;
  uint8_t *buffer = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(input_index), &capacity);
  if (buffer == nullptr || capacity == 0) {
    LOGE("MediaCodecPlayer: RTMP getInputBuffer failed idx=%zd capacity=%zu",
         input_index, capacity);
    return false;
  }

  const int rc = ReadFilteredRtmpPacket(rtmp);
  if (rc < 0) {
    if (g_stop.load(std::memory_order_acquire)) return true;
    LOGE("MediaCodecPlayer: RTMP read packet failed rc=%d (%s) source=%s",
         rc, AvErrorString(rc).c_str(), rtmp->url_redacted.c_str());
    return false;
  }

  AVPacket *packet = rtmp->filtered;
  if (packet == nullptr || packet->size <= 0 || packet->data == nullptr) {
    LOGE("MediaCodecPlayer: RTMP empty packet failed source=%s",
         rtmp->url_redacted.c_str());
    return false;
  }
  if (static_cast<size_t>(packet->size) > capacity) {
    LOGE("MediaCodecPlayer: RTMP packet too large size=%d capacity=%zu source=%s",
         packet->size, capacity, rtmp->url_redacted.c_str());
    return false;
  }
  memcpy(buffer, packet->data, static_cast<size_t>(packet->size));
  const int64_t pts_us = RtmpPacketPtsUs(rtmp, packet);
  const media_status_t status = AMediaCodec_queueInputBuffer(
      codec, static_cast<size_t>(input_index), 0, static_cast<size_t>(packet->size),
      static_cast<uint64_t>(std::max<int64_t>(0, pts_us)), 0);
  if (status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: RTMP queueInputBuffer failed status=%d size=%d pts=%lld",
         status, packet->size, static_cast<long long>(pts_us));
    return false;
  }
  const uint64_t count = queued_rtmp_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ShouldLogCounter(count, 5, 120)) {
    LOGI("MediaCodecPlayer: RTMP queued input #%llu idx=%zd size=%d cap=%zu pts=%lld key=%d",
         static_cast<unsigned long long>(count), input_index, packet->size, capacity,
         static_cast<long long>(pts_us), (packet->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0);
  }
  av_packet_unref(packet);
  return true;
}

DecodeOutcome DecodeOnce(const Options &opt, BinderClient *binder,
                         std::string *switch_input) {
  AMediaExtractor *extractor = nullptr;
  AMediaCodec *codec = nullptr;
  int input_fd = -1;
  TrackInfo track{};
  OutputLayout layout{};
  SourceRing ring;
  bool ring_registered = false;
  bool stream_logged = false;
  bool codec_started = false;

  auto cleanup = [&]() {
    if (codec != nullptr) {
      if (codec_started) AMediaCodec_stop(codec);
      AMediaCodec_delete(codec);
    }
    if (track.format != nullptr) AMediaFormat_delete(track.format);
    if (extractor != nullptr) AMediaExtractor_delete(extractor);
    if (input_fd >= 0) close(input_fd);
  };

  auto ensure_ring_registered = [&]() -> bool {
    if (layout.width <= 0 || layout.height <= 0) {
      LOGE("MediaCodecPlayer: source ring init failed invalid layout %dx%d",
           layout.width, layout.height);
      return false;
    }
    if (ring_registered) {
      if (layout.width != ring.width || layout.height != ring.height) {
        LOGE("MediaCodecPlayer: output size changed after registration failed old=%dx%d new=%dx%d",
             ring.width, ring.height, layout.width, layout.height);
        return false;
      }
      return true;
    }
    if (!InitSourceRing(&ring, layout.width, layout.height) ||
        !RegisterSourceRing(binder, ring)) {
      return false;
    }
    ring_registered = true;
    if (!stream_logged) {
      LOGI("MediaCodecPlayer: stream=%zu mime=%s src=%dx%d out=%dx%d fps=%d fpsCap=%d durationUs=%lld input=%s",
           track.index, track.mime.c_str(), track.width, track.height, layout.width,
           layout.height, track.frame_rate, opt.fps_cap,
           static_cast<long long>(track.duration_us), opt.input.c_str());
      stream_logged = true;
    }
    return true;
  };

  LOGI("MediaCodecPlayer: open input %s", opt.input.c_str());
  if (!OpenExtractor(opt.input, &extractor, &input_fd)) {
    cleanup();
    return DecodeOutcome::kFailed;
  }
  if (!FindVideoTrack(extractor, &track)) {
    cleanup();
    return DecodeOutcome::kFailed;
  }
  if (AMediaExtractor_selectTrack(extractor, track.index) != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: selectTrack failed index=%zu", track.index);
    cleanup();
    return DecodeOutcome::kFailed;
  }

  codec = AMediaCodec_createDecoderByType(track.mime.c_str());
  if (codec == nullptr) {
    LOGE("MediaCodecPlayer: createDecoderByType failed mime=%s", track.mime.c_str());
    cleanup();
    return DecodeOutcome::kFailed;
  }
  const media_status_t configure_status = AMediaCodec_configure(codec, track.format,
                                                               nullptr, nullptr, 0);
  if (configure_status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: configure failed status=%d mime=%s format=%s",
         configure_status, track.mime.c_str(), AMediaFormat_toString(track.format));
    cleanup();
    return DecodeOutcome::kFailed;
  }
  const media_status_t start_status = AMediaCodec_start(codec);
  if (start_status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: start failed status=%d mime=%s", start_status, track.mime.c_str());
    cleanup();
    return DecodeOutcome::kFailed;
  }
  codec_started = true;

  if (AMediaFormat *initial_output_format = AMediaCodec_getOutputFormat(codec);
      initial_output_format != nullptr) {
    if (UpdateOutputLayout(initial_output_format, track.width, track.height, &layout, false)) {
      if (!ensure_ring_registered()) {
        AMediaFormat_delete(initial_output_format);
        cleanup();
        return DecodeOutcome::kFailed;
      }
    } else {
      LOGW("MediaCodecPlayer: initial output format incomplete; waiting for codec output format");
    }
    AMediaFormat_delete(initial_output_format);
  }

  bool input_eos = false;
  bool output_eos = false;
  bool clock_started = false;
  int64_t base_wall_us = 0;
  int64_t base_pts_us = 0;
  int64_t last_published_pts_us = -1;
  uint64_t decoded = 0;
  uint64_t dropped = 0;
  int64_t fps_window_us = MonotonicUs();
  int64_t variant_probe_us = fps_window_us;
  uint64_t fps_frames = 0;

  while (!g_stop.load(std::memory_order_acquire) && !output_eos) {
    const int64_t probe_now_us = MonotonicUs();
    if (opt.auto_variant && probe_now_us - variant_probe_us >= 1000000LL) {
      variant_probe_us = probe_now_us;
      const std::string wanted = ResolveAutoVariantInput(binder, opt.input, "poll");
      if (!wanted.empty() && wanted != opt.input && IsReadableRegularFile(wanted)) {
        if (switch_input != nullptr) *switch_input = wanted;
        LOGI("MediaCodecPlayer: switching input current=%s next=%s",
             opt.input.c_str(), wanted.c_str());
        cleanup();
        return DecodeOutcome::kSwitchInput;
      }
    }

    if (!QueueExtractorInput(codec, extractor, &input_eos)) {
      cleanup();
      return DecodeOutcome::kFailed;
    }

    AMediaCodecBufferInfo info{};
    const ssize_t output_index = AMediaCodec_dequeueOutputBuffer(codec, &info, kCodecIoTimeoutUs);
    if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      continue;
    }
    if (output_index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *new_format = AMediaCodec_getOutputFormat(codec);
      if (new_format == nullptr || !UpdateOutputLayout(new_format, track.width, track.height,
                                                       &layout)) {
        if (new_format != nullptr) AMediaFormat_delete(new_format);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      AMediaFormat_delete(new_format);
      if (!ensure_ring_registered()) {
        cleanup();
        return DecodeOutcome::kFailed;
      }
      continue;
    }
    if (output_index < 0) {
      LOGE("MediaCodecPlayer: dequeueOutputBuffer failed rc=%zd", output_index);
      cleanup();
      return DecodeOutcome::kFailed;
    }

    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
      output_eos = true;
    }

    if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
      if (!ring_registered) {
        AMediaFormat *buffer_format =
            AMediaCodec_getBufferFormat(codec, static_cast<size_t>(output_index));
        if (buffer_format == nullptr ||
            !UpdateOutputLayout(buffer_format, track.width, track.height, &layout)) {
          if (buffer_format != nullptr) AMediaFormat_delete(buffer_format);
          LOGE("MediaCodecPlayer: output buffer format unavailable before first frame failed");
          AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
          cleanup();
          return DecodeOutcome::kFailed;
        }
        AMediaFormat_delete(buffer_format);
        if (!ensure_ring_registered()) {
          AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
          cleanup();
          return DecodeOutcome::kFailed;
        }
      }
      const int64_t pts_us = info.presentationTimeUs;
      if (!output_eos &&
          ShouldDropForFpsCap(opt, pts_us, clock_started, base_wall_us,
                              base_pts_us, last_published_pts_us)) {
        dropped += 1;
        if (ShouldLogCounter(dropped, 5, 120)) {
          LOGI("MediaCodecPlayer: dropped decoded frame #%llu cap=%d pts=%lld lastPts=%lld input=%s",
               static_cast<unsigned long long>(dropped), opt.fps_cap,
               static_cast<long long>(pts_us),
               static_cast<long long>(last_published_pts_us),
               opt.input.c_str());
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        continue;
      }
      size_t output_size = 0;
      uint8_t *output = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(output_index),
                                                    &output_size);
      if (output == nullptr || static_cast<size_t>(info.offset) > output_size ||
          static_cast<size_t>(info.size) > output_size - static_cast<size_t>(info.offset)) {
        LOGE("MediaCodecPlayer: getOutputBuffer failed idx=%zd offset=%d size=%d cap=%zu",
             output_index, info.offset, info.size, output_size);
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      const uint8_t *frame_ptr = output + info.offset;
      const size_t frame_size = static_cast<size_t>(info.size);
      RingWriteSlot write{};
      if (!BeginWriteFrame(&ring, pts_us, &write) || write.data == nullptr) {
        LOGE("MediaCodecPlayer: begin source ring write failed");
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      if (!ConvertOutputToI420Buffer(frame_ptr, frame_size, layout, write.data,
                                     ring.slot_size)) {
        LOGE("MediaCodecPlayer: output conversion failed layout=%s color=%#x out=%dx%d stride=%d slice=%d size=%zu",
             LayoutKindName(layout.kind), layout.color_format, layout.width, layout.height,
             layout.stride, layout.slice_height, frame_size);
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      PaceFrame(pts_us, &clock_started, &base_wall_us, &base_pts_us);
      FinishWriteFrame(&ring, write);
      last_published_pts_us = pts_us;
      decoded += 1;
      fps_frames += 1;
      if (ShouldLogCounter(decoded, 5, 120)) {
        LOGI("MediaCodecPlayer: wrote source frame #%llu gen=%llu slot=%u out=%dx%d pts=%lld",
             static_cast<unsigned long long>(decoded),
             static_cast<unsigned long long>(write.generation), write.slot_index,
             layout.width, layout.height, static_cast<long long>(pts_us));
      }
      const int64_t now_us = MonotonicUs();
      if (now_us - fps_window_us >= 2000000LL) {
        const double fps = static_cast<double>(fps_frames) * 1000000.0 /
                           static_cast<double>(now_us - fps_window_us);
        LOGI("MediaCodecPlayer: decoded FPS fps=%.1f frames=%llu latestGen=%llu input=%s",
             fps, static_cast<unsigned long long>(fps_frames),
             static_cast<unsigned long long>(ring.next_generation),
             opt.input.c_str());
        fps_window_us = now_us;
        fps_frames = 0;
      }
    }

    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
  }

  if (ring_registered) {
    LOGI("MediaCodecPlayer: decode pass complete frames=%llu dropped=%llu eos=%d stop=%d",
         static_cast<unsigned long long>(decoded),
         static_cast<unsigned long long>(dropped), output_eos ? 1 : 0,
         g_stop.load(std::memory_order_acquire) ? 1 : 0);
  }
  cleanup();
  return g_stop.load(std::memory_order_acquire) ? DecodeOutcome::kStopped
                                                : DecodeOutcome::kEof;
}

DecodeOutcome DecodeRtmpOnce(const Options &opt, BinderClient *binder) {
  AMediaCodec *codec = nullptr;
  AMediaFormat *input_format = nullptr;
  RtmpInput rtmp;
  OutputLayout layout{};
  SourceRing ring;
  bool ring_registered = false;
  bool stream_logged = false;
  bool codec_started = false;

  auto cleanup = [&]() {
    if (codec != nullptr) {
      if (codec_started) AMediaCodec_stop(codec);
      AMediaCodec_delete(codec);
    }
    if (input_format != nullptr) AMediaFormat_delete(input_format);
  };

  auto ensure_ring_registered = [&]() -> bool {
    if (layout.width <= 0 || layout.height <= 0) {
      LOGE("MediaCodecPlayer: RTMP source ring init failed invalid layout %dx%d",
           layout.width, layout.height);
      return false;
    }
    if (ring_registered) {
      if (layout.width != ring.width || layout.height != ring.height) {
        LOGE("MediaCodecPlayer: RTMP output size changed after registration failed old=%dx%d new=%dx%d",
             ring.width, ring.height, layout.width, layout.height);
        return false;
      }
      return true;
    }
    if (!InitSourceRing(&ring, layout.width, layout.height) ||
        !RegisterSourceRing(binder, ring)) {
      return false;
    }
    ring_registered = true;
    if (!stream_logged) {
      LOGI("MediaCodecPlayer: RTMP stream mime=%s src=%dx%d out=%dx%d fps=%d fpsCap=%d source=%s",
           rtmp.mime.c_str(), rtmp.width, rtmp.height, layout.width, layout.height,
           rtmp.frame_rate, opt.fps_cap, rtmp.url_redacted.c_str());
      stream_logged = true;
    }
    return true;
  };

  LOGI("MediaCodecPlayer: open RTMP URL file %s", opt.rtmp_url_file.c_str());
  if (!OpenRtmpInput(opt.rtmp_url_file, &rtmp, &input_format)) {
    cleanup();
    return DecodeOutcome::kFailed;
  }

  codec = AMediaCodec_createDecoderByType(rtmp.mime.c_str());
  if (codec == nullptr) {
    LOGE("MediaCodecPlayer: RTMP createDecoderByType failed mime=%s",
         rtmp.mime.c_str());
    cleanup();
    return DecodeOutcome::kFailed;
  }
  const media_status_t configure_status = AMediaCodec_configure(codec, input_format,
                                                               nullptr, nullptr, 0);
  if (configure_status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: RTMP configure failed status=%d mime=%s format=%s",
         configure_status, rtmp.mime.c_str(), AMediaFormat_toString(input_format));
    cleanup();
    return DecodeOutcome::kFailed;
  }
  LOGI("MediaCodecPlayer: configured RTMP MediaCodec mime=%s src=%dx%d fps=%d",
       rtmp.mime.c_str(), rtmp.width, rtmp.height, rtmp.frame_rate);
  const media_status_t start_status = AMediaCodec_start(codec);
  if (start_status != AMEDIA_OK) {
    LOGE("MediaCodecPlayer: RTMP start failed status=%d mime=%s",
         start_status, rtmp.mime.c_str());
    cleanup();
    return DecodeOutcome::kFailed;
  }
  codec_started = true;

  if (AMediaFormat *initial_output_format = AMediaCodec_getOutputFormat(codec);
      initial_output_format != nullptr) {
    if (UpdateOutputLayout(initial_output_format, rtmp.width, rtmp.height, &layout, false)) {
      if (!ensure_ring_registered()) {
        AMediaFormat_delete(initial_output_format);
        cleanup();
        return DecodeOutcome::kFailed;
      }
    } else {
      LOGW("MediaCodecPlayer: RTMP initial output format incomplete; waiting for codec output format");
    }
    AMediaFormat_delete(initial_output_format);
  }

  bool output_eos = false;
  bool clock_started = false;
  int64_t base_wall_us = 0;
  int64_t base_pts_us = 0;
  int64_t last_published_pts_us = -1;
  uint64_t decoded = 0;
  uint64_t dropped = 0;
  int64_t fps_window_us = MonotonicUs();
  uint64_t fps_frames = 0;

  while (!g_stop.load(std::memory_order_acquire) && !output_eos) {
    if (!QueueRtmpInput(codec, &rtmp)) {
      cleanup();
      return DecodeOutcome::kFailed;
    }

    AMediaCodecBufferInfo info{};
    const ssize_t output_index = AMediaCodec_dequeueOutputBuffer(codec, &info, kCodecIoTimeoutUs);
    if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      continue;
    }
    if (output_index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *new_format = AMediaCodec_getOutputFormat(codec);
      if (new_format == nullptr || !UpdateOutputLayout(new_format, rtmp.width, rtmp.height,
                                                       &layout)) {
        if (new_format != nullptr) AMediaFormat_delete(new_format);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      AMediaFormat_delete(new_format);
      if (!ensure_ring_registered()) {
        cleanup();
        return DecodeOutcome::kFailed;
      }
      continue;
    }
    if (output_index < 0) {
      LOGE("MediaCodecPlayer: RTMP dequeueOutputBuffer failed rc=%zd", output_index);
      cleanup();
      return DecodeOutcome::kFailed;
    }

    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
      output_eos = true;
    }

    if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
      if (!ring_registered) {
        AMediaFormat *buffer_format =
            AMediaCodec_getBufferFormat(codec, static_cast<size_t>(output_index));
        if (buffer_format == nullptr ||
            !UpdateOutputLayout(buffer_format, rtmp.width, rtmp.height, &layout)) {
          if (buffer_format != nullptr) AMediaFormat_delete(buffer_format);
          LOGE("MediaCodecPlayer: RTMP output buffer format unavailable before first frame failed");
          AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
          cleanup();
          return DecodeOutcome::kFailed;
        }
        AMediaFormat_delete(buffer_format);
        if (!ensure_ring_registered()) {
          AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
          cleanup();
          return DecodeOutcome::kFailed;
        }
      }
      const int64_t pts_us = info.presentationTimeUs;
      if (!output_eos &&
          ShouldDropForFpsCap(opt, pts_us, clock_started, base_wall_us,
                              base_pts_us, last_published_pts_us)) {
        dropped += 1;
        if (ShouldLogCounter(dropped, 5, 120)) {
          LOGI("MediaCodecPlayer: RTMP dropped decoded frame #%llu cap=%d pts=%lld lastPts=%lld",
               static_cast<unsigned long long>(dropped), opt.fps_cap,
               static_cast<long long>(pts_us),
               static_cast<long long>(last_published_pts_us));
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        continue;
      }
      size_t output_size = 0;
      uint8_t *output = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(output_index),
                                                    &output_size);
      if (output == nullptr || static_cast<size_t>(info.offset) > output_size ||
          static_cast<size_t>(info.size) > output_size - static_cast<size_t>(info.offset)) {
        LOGE("MediaCodecPlayer: RTMP getOutputBuffer failed idx=%zd offset=%d size=%d cap=%zu",
             output_index, info.offset, info.size, output_size);
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      const uint8_t *frame_ptr = output + info.offset;
      const size_t frame_size = static_cast<size_t>(info.size);
      RingWriteSlot write{};
      if (!BeginWriteFrame(&ring, pts_us, &write) || write.data == nullptr) {
        LOGE("MediaCodecPlayer: RTMP begin source ring write failed");
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      if (!ConvertOutputToI420Buffer(frame_ptr, frame_size, layout, write.data,
                                     ring.slot_size)) {
        LOGE("MediaCodecPlayer: RTMP output conversion failed layout=%s color=%#x out=%dx%d stride=%d slice=%d size=%zu",
             LayoutKindName(layout.kind), layout.color_format, layout.width, layout.height,
             layout.stride, layout.slice_height, frame_size);
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
        cleanup();
        return DecodeOutcome::kFailed;
      }
      // RTMP is already paced by network/demux arrival. Do not add MP4-style
      // sleep here; it can increase live latency after network jitter.
      FinishWriteFrame(&ring, write);
      last_published_pts_us = pts_us;
      decoded += 1;
      fps_frames += 1;
      if (ShouldLogCounter(decoded, 5, 120)) {
        LOGI("MediaCodecPlayer: RTMP wrote source frame #%llu gen=%llu slot=%u out=%dx%d pts=%lld",
             static_cast<unsigned long long>(decoded),
             static_cast<unsigned long long>(write.generation), write.slot_index,
             layout.width, layout.height, static_cast<long long>(pts_us));
      }
      const int64_t now_us = MonotonicUs();
      if (now_us - fps_window_us >= 2000000LL) {
        const double fps = static_cast<double>(fps_frames) * 1000000.0 /
                           static_cast<double>(now_us - fps_window_us);
        LOGI("MediaCodecPlayer: decoded FPS fps=%.1f frames=%llu latestGen=%llu source=rtmp",
             fps, static_cast<unsigned long long>(fps_frames),
             static_cast<unsigned long long>(ring.next_generation));
        fps_window_us = now_us;
        fps_frames = 0;
      }
    }

    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output_index), false);
  }

  if (ring_registered) {
    LOGI("MediaCodecPlayer: RTMP decode pass complete frames=%llu dropped=%llu eos=%d stop=%d",
         static_cast<unsigned long long>(decoded),
         static_cast<unsigned long long>(dropped), output_eos ? 1 : 0,
         g_stop.load(std::memory_order_acquire) ? 1 : 0);
  }
  cleanup();
  return g_stop.load(std::memory_order_acquire) ? DecodeOutcome::kStopped
                                                : DecodeOutcome::kEof;
}

}  // namespace

int main(int argc, char **argv) {
  signal(SIGTERM, SignalHandler);
  signal(SIGINT, SignalHandler);
  TuneCurrentThreadPriority("main");

  Options opt = ParseOptions(argc, argv);
  const bool rtmp_mode = !opt.rtmp_url_file.empty();
  if (opt.input.empty() == opt.rtmp_url_file.empty()) {
    LOGE("MediaCodecPlayer: invalid source selection; choose exactly one of --input or --rtmp-url-file");
    return 1;
  }
  WritePidFile(opt.pidfile);

  BinderClient binder{};
  if (!ConnectService(&binder)) {
    if (!opt.pidfile.empty()) unlink(opt.pidfile.c_str());
    return 2;
  }

  uint64_t loops = 0;
  int exit_code = 0;
  if (rtmp_mode) {
    LOGI("MediaCodecPlayer: initial source=rtmp urlFile=%s fpsCap=%d live=1",
         opt.rtmp_url_file.c_str(), opt.fps_cap);
    const DecodeOutcome outcome = DecodeRtmpOnce(opt, &binder);
    if (outcome == DecodeOutcome::kFailed && !g_stop.load(std::memory_order_acquire)) {
      LOGE("MediaCodecPlayer: RTMP decode failed urlFile=%s", opt.rtmp_url_file.c_str());
      exit_code = 3;
    }
    ClearService(&binder);
    if (!opt.pidfile.empty()) unlink(opt.pidfile.c_str());
    LOGI("MediaCodecPlayer: exit code=%d", exit_code);
    return exit_code;
  }

  std::string current_input = opt.auto_variant
                                  ? ResolveAutoVariantInput(&binder, opt.input, "start")
                                  : opt.input;
  LOGI("MediaCodecPlayer: initial input=%s autoVariant=%d fpsCap=%d default=%s",
       current_input.c_str(), opt.auto_variant ? 1 : 0, opt.fps_cap,
       opt.input.c_str());
  do {
    loops += 1;
    Options pass = opt;
    pass.input = current_input;
    std::string switch_input;
    const DecodeOutcome outcome = DecodeOnce(pass, &binder, &switch_input);
    if (outcome == DecodeOutcome::kSwitchInput && !switch_input.empty() &&
        !g_stop.load(std::memory_order_acquire)) {
      current_input = switch_input;
      LOGI("MediaCodecPlayer: restarting for target variant input=%s",
           current_input.c_str());
      continue;
    }
    if (outcome == DecodeOutcome::kFailed && !g_stop.load(std::memory_order_acquire)) {
      LOGE("MediaCodecPlayer: decode failed input=%s", current_input.c_str());
      exit_code = 3;
      break;
    }
    if (opt.loop && !g_stop.load(std::memory_order_acquire)) {
      if (opt.auto_variant) {
        current_input = ResolveAutoVariantInput(&binder, opt.input, "loop");
      }
      LOGI("MediaCodecPlayer: loop EOF reached, restarting input loops=%llu",
           static_cast<unsigned long long>(loops));
    }
  } while (opt.loop && !g_stop.load(std::memory_order_acquire));

  ClearService(&binder);
  if (!opt.pidfile.empty()) unlink(opt.pidfile.c_str());
  LOGI("MediaCodecPlayer: exit code=%d", exit_code);
  return exit_code;
}
