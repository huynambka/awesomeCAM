#include "video2camera_service.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "video2camera_ipc.h"
#include "video2camera_ndk.h"
#include "ready_frame_cache.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

struct TargetStore {
  std::mutex mutex;
  std::vector<VideoTargetState> targets;
  uint64_t next_generation = 0;
};

struct SharedMemoryMapping {
  void *addr = nullptr;
  size_t size = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t frame_format = kFrameFormatUnknown;
  uint32_t slot_count = 0;
  uint32_t slot_size = 0;
  uint64_t registration_generation = 0;

  ~SharedMemoryMapping() {
    if (addr != nullptr && addr != MAP_FAILED && size > 0) {
      munmap(addr, size);
    }
  }

  SharedMemoryRingHeader *header() const {
    return reinterpret_cast<SharedMemoryRingHeader *>(addr);
  }
};

struct SharedMemoryStore {
  std::mutex mutex;
  std::shared_ptr<SharedMemoryMapping> source;
  uint64_t registration_generation = 0;
};

TargetStore g_target_store;
SharedMemoryStore g_memory_store;
BinderRuntimeApi g_binder_runtime;
std::atomic<bool> g_service_started{false};
std::atomic<bool> g_service_registered{false};
std::atomic<bool> g_service_launching{false};
std::atomic<bool> g_probe_c51_launching{false};
std::atomic<bool> g_probe_c52_launching{false};
AIBinder_Class *g_service_class = nullptr;
AIBinder *g_service_binder = nullptr;

struct ClassicBinderRuntimeApi {
  void *handle = nullptr;
  void *sym_process_state_self = nullptr;
  void *sym_process_state_start_thread_pool = nullptr;
  void *sym_ipc_thread_state_self = nullptr;
  void *sym_ipc_thread_state_join_thread_pool = nullptr;
};

ClassicBinderRuntimeApi g_classic_binder_runtime;
std::atomic<bool> g_probe_c61_launching{false};
std::atomic<bool> g_probe_c62_launching{false};

struct OpaqueStrongPointer {
  void *ptr = nullptr;
};

constexpr uint64_t kNsPerSecond = 1000000000ULL;
constexpr uint64_t kTargetTtlNs = 5ULL * kNsPerSecond;
constexpr size_t kMaxTrackedTargets = 16;
constexpr int32_t kHalPixelFormatImplementationDefined = 0x22;
constexpr int32_t kHalPixelFormatYcbcr420888 = 0x23;

using FnProcessStateStartThreadPool = void (*)(void *);
using FnIPCThreadStateSelf = void *(*)();
using FnIPCThreadStateJoinThreadPool = void (*)(void *, bool);

template <typename T>
T AtomicLoadAcquire(const T *ptr) {
  T value{};
  __atomic_load(ptr, &value, __ATOMIC_ACQUIRE);
  return value;
}

uint64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool IsSupportedTargetFormat(int32_t format) {
  return format == kHalPixelFormatYcbcr420888 ||
         format == kHalPixelFormatImplementationDefined;
}

bool SameTargetKey(const VideoTargetState &target, int32_t width, int32_t height,
                   int32_t format) {
  return target.width == width && target.height == height && target.format == format;
}

void PruneExpiredTargetsLocked(uint64_t now_ns) {
  auto &targets = g_target_store.targets;
  const size_t before = targets.size();
  targets.erase(std::remove_if(targets.begin(), targets.end(),
                               [&](const VideoTargetState &target) {
                                 return target.last_seen_ns != 0 && now_ns > target.last_seen_ns &&
                                        now_ns - target.last_seen_ns > kTargetTtlNs;
                               }),
                targets.end());
  if (targets.size() != before) {
    LOGI("Video2CameraService expired %zu inactive target(s), active=%zu",
         before - targets.size(), targets.size());
  }
}

bool LoadClassicBinderRuntimeApi(ClassicBinderRuntimeApi *api) {
  if (api == nullptr) return false;
  if (api->handle != nullptr) {
    return api->sym_process_state_self != nullptr &&
           api->sym_process_state_start_thread_pool != nullptr &&
           api->sym_ipc_thread_state_self != nullptr &&
           api->sym_ipc_thread_state_join_thread_pool != nullptr;
  }
  api->handle = dlopen("libbinder.so", RTLD_NOW | RTLD_LOCAL);
  if (api->handle == nullptr) {
    LOGE("ClassicBinder: dlopen(libbinder.so) failed: %s",
         dlerror() ? dlerror() : "unknown");
    return false;
  }
  api->sym_process_state_self = dlsym(api->handle, "_ZN7android12ProcessState4selfEv");
  api->sym_process_state_start_thread_pool =
      dlsym(api->handle, "_ZN7android12ProcessState15startThreadPoolEv");
  api->sym_ipc_thread_state_self = dlsym(api->handle, "_ZN7android14IPCThreadState4selfEv");
  api->sym_ipc_thread_state_join_thread_pool =
      dlsym(api->handle, "_ZN7android14IPCThreadState14joinThreadPoolEb");
  const bool ok = api->sym_process_state_self != nullptr &&
                  api->sym_process_state_start_thread_pool != nullptr &&
                  api->sym_ipc_thread_state_self != nullptr &&
                  api->sym_ipc_thread_state_join_thread_pool != nullptr;
  if (!ok) {
    LOGE("ClassicBinder: missing required symbols self=%p start=%p ipcSelf=%p join=%p",
         api->sym_process_state_self, api->sym_process_state_start_thread_pool,
         api->sym_ipc_thread_state_self, api->sym_ipc_thread_state_join_thread_pool);
  }
  return ok;
}

bool CallHiddenResultNoArg(void *fn, void *out) {
#if defined(__aarch64__)
  asm volatile(
      "mov x8, %x[out]\n"
      "blr %x[fn]\n"
      :
      : [fn] "r"(fn), [out] "r"(out)
      : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
        "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory", "cc");
  return true;
#else
  (void)fn;
  (void)out;
  return false;
#endif
}

bool GetClassicProcessState(OpaqueStrongPointer *out) {
  if (out == nullptr) return false;
  out->ptr = nullptr;
  if (!LoadClassicBinderRuntimeApi(&g_classic_binder_runtime)) return false;
  if (!CallHiddenResultNoArg(g_classic_binder_runtime.sym_process_state_self, out)) return false;
  return out->ptr != nullptr;
}

void StartClassicBinderThreadPool() {
  OpaqueStrongPointer process_state{};
  if (!GetClassicProcessState(&process_state)) {
    LOGE("ClassicBinder: ProcessState::self() failed");
    return;
  }
  LOGI("ClassicBinder: ProcessState ptr=%p", process_state.ptr);
  reinterpret_cast<FnProcessStateStartThreadPool>(
      g_classic_binder_runtime.sym_process_state_start_thread_pool)(process_state.ptr);
  LOGI("ClassicBinder: ProcessState::startThreadPool() returned");
  void *ipc = reinterpret_cast<FnIPCThreadStateSelf>(
      g_classic_binder_runtime.sym_ipc_thread_state_self)();
  LOGI("ClassicBinder: IPCThreadState ptr=%p", ipc);
  if (ipc == nullptr) {
    LOGE("ClassicBinder: IPCThreadState::self() failed");
    return;
  }
  reinterpret_cast<FnIPCThreadStateJoinThreadPool>(
      g_classic_binder_runtime.sym_ipc_thread_state_join_thread_pool)(ipc, true);
  LOGI("ClassicBinder: IPCThreadState::joinThreadPool() returned");
}

bool ValidateSharedMemoryHeader(const SharedMemoryRingHeader *header, int32_t width,
                                int32_t height, int32_t format, uint32_t slot_count,
                                uint32_t slot_size, size_t region_size) {
  if (header == nullptr) return false;
  if (header->magic != kSharedMemoryRingMagic ||
      header->version != kSharedMemoryRingVersion ||
      header->header_size != sizeof(SharedMemoryRingHeader) ||
      header->slot_count != slot_count || header->slot_count != kSharedMemoryRingSlotCount ||
      header->width != width || header->height != height || header->format != format ||
      header->format != kFrameFormatI420 || header->slot_size != slot_size) {
    return false;
  }
  const size_t expected_slot_size = I420FrameSize(width, height);
  const size_t expected_region_size = SharedMemoryRingSize(width, height);
  return expected_slot_size == slot_size && expected_region_size <= region_size;
}

bool RegisterSourceMemoryFromFd(int fd, int32_t width, int32_t height,
                                int32_t frame_format, uint32_t slot_count,
                                uint32_t slot_size, size_t region_size) {
  if (fd < 0 || width <= 0 || height <= 0 || frame_format != kFrameFormatI420 ||
      slot_count != kSharedMemoryRingSlotCount || slot_size == 0 || region_size == 0) {
    LOGW("Video2CameraService reject source fd=%d src=%dx%d frameFmt=%d slots=%u slot=%u region=%zu",
         fd, width, height, frame_format, slot_count, slot_size, region_size);
    return false;
  }

  struct stat st {};
  if (fstat(fd, &st) == 0 && st.st_size > 0 && static_cast<size_t>(st.st_size) < region_size) {
    LOGW("Video2CameraService reject source fd size=%lld region=%zu",
         static_cast<long long>(st.st_size), region_size);
    return false;
  }

  void *addr = mmap(nullptr, region_size, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    LOGE("Video2CameraService mmap source failed errno=%d (%s)", errno, strerror(errno));
    return false;
  }

  auto *header = reinterpret_cast<SharedMemoryRingHeader *>(addr);
  if (!ValidateSharedMemoryHeader(header, width, height, frame_format, slot_count, slot_size,
                                  region_size)) {
    LOGW("Video2CameraService reject source header magic=%#x ver=%u hdr=%u %dx%d fmt=%d slots=%u slot=%u latest=%llu region=%zu",
         header->magic, header->version, header->header_size, header->width,
         header->height, header->format, header->slot_count, header->slot_size,
         static_cast<unsigned long long>(header->latest_generation), region_size);
    munmap(addr, region_size);
    return false;
  }

  auto mapping = std::make_shared<SharedMemoryMapping>();
  mapping->addr = addr;
  mapping->size = region_size;
  mapping->width = width;
  mapping->height = height;
  mapping->frame_format = frame_format;
  mapping->slot_count = slot_count;
  mapping->slot_size = slot_size;
  {
    std::lock_guard<std::mutex> lock(g_memory_store.mutex);
    mapping->registration_generation = ++g_memory_store.registration_generation;
    g_memory_store.source = mapping;
  }

  LOGI("Video2CameraService source ring registered regGen=%llu src=%dx%d frameFmt=%d slots=%u slot=%u region=%zu addr=%p",
       static_cast<unsigned long long>(mapping->registration_generation), width, height,
       frame_format, slot_count, slot_size, region_size, addr);
  return true;
}

void ClearSharedMemoryRegistrationLocked() {
  g_memory_store.source.reset();
  g_memory_store.registration_generation += 1;
}

void *OnCreate(void *) { return nullptr; }
void OnDestroy(void *) {}

binder_status_t WriteTargetsToParcel(AParcel *out) {
  std::vector<VideoTargetState> targets;
  GetVideo2CameraTargets(&targets);
  binder_status_t status =
      g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(targets.size()));
  for (const VideoTargetState &target : targets) {
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, target.width);
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, target.height);
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, target.format);
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, static_cast<int64_t>(target.generation));
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, static_cast<int64_t>(target.last_seen_ns));
    if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, static_cast<int64_t>(target.hit_count));
    if (status != STATUS_OK) break;
  }
  return status;
}

binder_status_t WriteSourceStatusToParcel(AParcel *out) {
  SourceFrameStatus state{};
  GetSourceFrameStatus(&state);
  binder_status_t status = g_binder_runtime.parcel_write_int32(out, state.width);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, state.height);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, state.format);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, static_cast<int64_t>(state.generation));
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(state.slot));
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, state.pts_us);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int64(out, static_cast<int64_t>(state.registration_generation));
  return status;
}

binder_status_t OnTransact(AIBinder *, transaction_code_t code, const AParcel *in,
                           AParcel *out) {
  switch (code) {
    case kTxnGetTargets: {
      if (out == nullptr) return STATUS_BAD_VALUE;
      return WriteTargetsToParcel(out);
    }
    case kTxnRegisterSourceMemory: {
      int fd = -1;
      int32_t width = 0;
      int32_t height = 0;
      int32_t frame_format = 0;
      int32_t slot_count_i32 = 0;
      int32_t slot_size_i32 = 0;
      int64_t region_size_i64 = 0;
      binder_status_t status = g_binder_runtime.parcel_read_fd(in, &fd);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int32(in, &width);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int32(in, &height);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int32(in, &frame_format);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int32(in, &slot_count_i32);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int32(in, &slot_size_i32);
      if (status == STATUS_OK) status = g_binder_runtime.parcel_read_int64(in, &region_size_i64);
      if (status != STATUS_OK) {
        if (fd >= 0) close(fd);
        return status;
      }
      const bool ok = RegisterSourceMemoryFromFd(
          fd, width, height, frame_format, static_cast<uint32_t>(slot_count_i32),
          static_cast<uint32_t>(slot_size_i32),
          region_size_i64 > 0 ? static_cast<size_t>(region_size_i64) : 0u);
      if (fd >= 0) close(fd);
      if (!ok) return STATUS_BAD_VALUE;
      if (out != nullptr) g_binder_runtime.parcel_write_int32(out, 1);
      return STATUS_OK;
    }
    case kTxnUnregisterSourceMemory: {
      ClearSharedMemoryRegistration();
      if (out != nullptr) g_binder_runtime.parcel_write_int32(out, 1);
      LOGI("Video2CameraService source ring unregistered");
      return STATUS_OK;
    }
    case kTxnClear: {
      ClearSharedMemoryRegistration();
      if (out != nullptr) g_binder_runtime.parcel_write_int32(out, 1);
      LOGI("Video2CameraService cleared source registration");
      return STATUS_OK;
    }
    case kTxnGetSourceStatus: {
      if (out == nullptr) return STATUS_BAD_VALUE;
      return WriteSourceStatusToParcel(out);
    }
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
}

bool EnsureServiceClass() {
  if (g_service_class != nullptr) return true;
  g_service_class = g_binder_runtime.binder_class_define(
      kVideo2CameraDescriptor, OnCreate, OnDestroy, OnTransact);
  return g_service_class != nullptr;
}

}  // namespace

void UpdateVideo2CameraTarget(int32_t width, int32_t height, int32_t format) {
  if (width <= 0 || height <= 0 || !IsSupportedTargetFormat(format)) return;
  const uint64_t now_ns = MonotonicNs();
  std::lock_guard<std::mutex> lock(g_target_store.mutex);
  PruneExpiredTargetsLocked(now_ns);
  for (VideoTargetState &target : g_target_store.targets) {
    if (!SameTargetKey(target, width, height, format)) continue;
    target.last_seen_ns = now_ns;
    target.hit_count += 1;
    if (target.hit_count <= 5 || (target.hit_count % 120) == 0) {
      LOGI("Video2CameraService target hit gen=%llu %dx%d fmt=%#x hits=%llu",
           static_cast<unsigned long long>(target.generation), width, height, format,
           static_cast<unsigned long long>(target.hit_count));
    }
    return;
  }

  if (g_target_store.targets.size() >= kMaxTrackedTargets) {
    auto oldest = std::min_element(
        g_target_store.targets.begin(), g_target_store.targets.end(),
        [](const VideoTargetState &a, const VideoTargetState &b) {
          return a.last_seen_ns < b.last_seen_ns;
        });
    if (oldest != g_target_store.targets.end()) {
      LOGI("Video2CameraService dropping oldest target gen=%llu %dx%d fmt=%#x",
           static_cast<unsigned long long>(oldest->generation), oldest->width,
           oldest->height, oldest->format);
      g_target_store.targets.erase(oldest);
    }
  }

  VideoTargetState state{};
  state.width = width;
  state.height = height;
  state.format = format;
  state.generation = ++g_target_store.next_generation;
  state.last_seen_ns = now_ns;
  state.hit_count = 1;
  g_target_store.targets.push_back(state);
  LOGI("Video2CameraService target active gen=%llu %dx%d fmt=%#x",
       static_cast<unsigned long long>(state.generation), width, height, format);
}

bool PeekVideo2CameraTarget(VideoTargetState *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_target_store.mutex);
  PruneExpiredTargetsLocked(MonotonicNs());
  if (g_target_store.targets.empty()) {
    *out = VideoTargetState{};
    return false;
  }
  auto latest = std::max_element(
      g_target_store.targets.begin(), g_target_store.targets.end(),
      [](const VideoTargetState &a, const VideoTargetState &b) {
        return a.last_seen_ns < b.last_seen_ns;
      });
  *out = latest != g_target_store.targets.end() ? *latest : VideoTargetState{};
  return out->width > 0 && out->height > 0;
}

bool GetVideo2CameraTargets(std::vector<VideoTargetState> *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_target_store.mutex);
  PruneExpiredTargetsLocked(MonotonicNs());
  *out = g_target_store.targets;
  std::sort(out->begin(), out->end(),
            [](const VideoTargetState &a, const VideoTargetState &b) {
              if (a.last_seen_ns != b.last_seen_ns) return a.last_seen_ns > b.last_seen_ns;
              const int64_t area_a = static_cast<int64_t>(a.width) * a.height;
              const int64_t area_b = static_cast<int64_t>(b.width) * b.height;
              return area_a > area_b;
            });
  return !out->empty();
}

bool CopyLatestSourceFrame(SharedMemoryFrameView *out) {
  if (out == nullptr) return false;
  *out = SharedMemoryFrameView{};

  std::shared_ptr<SharedMemoryMapping> mapping;
  {
    std::lock_guard<std::mutex> lock(g_memory_store.mutex);
    mapping = g_memory_store.source;
  }
  if (!mapping || mapping->frame_format != kFrameFormatI420) return false;

  const auto *header = mapping->header();
  if (!ValidateSharedMemoryHeader(header, mapping->width, mapping->height,
                                  mapping->frame_format, mapping->slot_count,
                                  mapping->slot_size, mapping->size)) {
    return false;
  }

  const uint64_t generation = AtomicLoadAcquire(&header->latest_generation);
  const uint32_t slot_index = AtomicLoadAcquire(&header->latest_slot);
  if (generation == 0 || slot_index >= header->slot_count || slot_index == kSharedMemoryNoSlot) {
    return false;
  }

  const SharedMemoryRingSlot &slot = header->slots[slot_index];
  const uint64_t begin_generation = AtomicLoadAcquire(&slot.begin_generation);
  const uint64_t end_generation = AtomicLoadAcquire(&slot.end_generation);
  const uint32_t data_offset = AtomicLoadAcquire(&slot.data_offset);
  const uint32_t data_size = AtomicLoadAcquire(&slot.data_size);
  const int64_t pts_us = AtomicLoadAcquire(&slot.pts_us);
  const size_t expected_size = I420FrameSize(mapping->width, mapping->height);

  if (begin_generation != generation || end_generation != generation ||
      data_size < expected_size || data_offset > mapping->size ||
      expected_size > mapping->size - data_offset) {
    return false;
  }

  out->bytes = static_cast<const uint8_t *>(mapping->addr) + data_offset;
  out->size = expected_size;
  out->width = mapping->width;
  out->height = mapping->height;
  out->format = kFrameFormatI420;
  out->generation = generation;
  out->slot = slot_index;
  out->pts_us = pts_us;
  out->keepalive = mapping;
  return true;
}

bool GetSourceFrameStatus(SourceFrameStatus *out) {
  if (out == nullptr) return false;
  *out = SourceFrameStatus{};
  std::shared_ptr<SharedMemoryMapping> mapping;
  {
    std::lock_guard<std::mutex> lock(g_memory_store.mutex);
    mapping = g_memory_store.source;
    out->registration_generation = g_memory_store.registration_generation;
  }
  if (!mapping) return false;
  out->width = mapping->width;
  out->height = mapping->height;
  out->format = mapping->frame_format;
  out->registration_generation = mapping->registration_generation;
  const auto *header = mapping->header();
  out->generation = AtomicLoadAcquire(&header->latest_generation);
  out->slot = AtomicLoadAcquire(&header->latest_slot);
  if (out->generation != 0 && out->slot < mapping->slot_count) {
    out->pts_us = AtomicLoadAcquire(&header->slots[out->slot].pts_us);
  }
  return true;
}

void ClearSharedMemoryRegistration() {
  {
    std::lock_guard<std::mutex> lock(g_memory_store.mutex);
    ClearSharedMemoryRegistrationLocked();
  }
  ClearReadyFrameCache();
}

bool ProbeBinderRuntimeOnly() {
  LOGI("Video2CameraService[C1]: loading binder runtime only");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C1]: failed to load binder runtime API");
    return false;
  }
  LOGI("Video2CameraService[C1]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  return true;
}

bool ProbeBinderClassDefineOnly() {
  LOGI("Video2CameraService[C2]: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) return false;
  LOGI("Video2CameraService[C2]: defining binder class only");
  return EnsureServiceClass();
}

bool ProbeBinderNewOnly() {
  LOGI("Video2CameraService[C3]: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime) || !EnsureServiceClass()) return false;
  AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  LOGI("Video2CameraService[C3]: binder instance=%p", binder);
  return binder != nullptr;
}

bool ProbeBinderThreadPoolOnly() {
  LOGI("Video2CameraService[C4]: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime) || !EnsureServiceClass()) return false;
  AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (binder == nullptr) return false;
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();
  LOGI("Video2CameraService[C4]: binder threadpool started");
  return true;
}

namespace {
void *ProbeC51WorkerMain(void *) {
  LOGI("Video2CameraService[C51]: worker start");
  if (LoadBinderRuntimeApi(&g_binder_runtime) && EnsureServiceClass()) {
    AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
    LOGI("Video2CameraService[C51]: binder instance=%p", binder);
    g_binder_runtime.set_thread_pool_max(1);
    g_binder_runtime.start_thread_pool();
    LOGI("Video2CameraService[C51]: binder threadpool started");
  }
  g_probe_c51_launching.store(false);
  return nullptr;
}

void *ProbeC52WorkerMain(void *) {
  LOGI("Video2CameraService[C52]: worker start");
  if (!LoadBinderRuntimeApi(&g_binder_runtime) || !EnsureServiceClass()) {
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status == STATUS_OK) {
    g_service_registered.store(true);
    LOGI("Video2CameraService[C52]: registered as %s", kVideo2CameraServiceName);
  } else {
    LOGE("Video2CameraService[C52]: add_service failed status=%d", add_status);
  }
  g_probe_c52_launching.store(false);
  return nullptr;
}

void *ProbeC61WorkerMain(void *) {
  LOGI("Video2CameraService[C61]: worker start");
  if (LoadClassicBinderRuntimeApi(&g_classic_binder_runtime)) {
    StartClassicBinderThreadPool();
  }
  g_probe_c61_launching.store(false);
  return nullptr;
}

void *ProbeC62WorkerMain(void *) {
  LOGI("Video2CameraService[C62]: worker start");
  if (!LoadBinderRuntimeApi(&g_binder_runtime) || !EnsureServiceClass()) {
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    LOGE("Video2CameraService[C62]: add_service failed status=%d", add_status);
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  g_service_registered.store(true);
  LOGI("Video2CameraService[C62]: registered as %s", kVideo2CameraServiceName);
  if (LoadClassicBinderRuntimeApi(&g_classic_binder_runtime)) {
    StartClassicBinderThreadPool();
  }
  g_probe_c62_launching.store(false);
  return nullptr;
}

bool LaunchDetachedWorker(std::atomic<bool> *flag, void *(*fn)(void *), const char *name) {
  bool expected = false;
  if (!flag->compare_exchange_strong(expected, true)) {
    LOGW("%s: worker already launching", name);
    return true;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, fn, nullptr);
  if (rc != 0) {
    LOGE("%s: pthread_create failed rc=%d", name, rc);
    flag->store(false);
    return false;
  }
  pthread_detach(thread);
  LOGI("%s: worker launched", name);
  return true;
}
}  // namespace

bool ProbeBinderThreadPoolWorkerOnly() {
  return LaunchDetachedWorker(&g_probe_c51_launching, ProbeC51WorkerMain,
                              "Video2CameraService[C51]");
}

bool ProbeBinderFullServiceWorkerAsync() {
  return LaunchDetachedWorker(&g_probe_c52_launching, ProbeC52WorkerMain,
                              "Video2CameraService[C52]");
}

bool ProbeClassicThreadPoolWorkerOnly() {
  return LaunchDetachedWorker(&g_probe_c61_launching, ProbeC61WorkerMain,
                              "Video2CameraService[C61]");
}

bool ProbeClassicServiceWorkerAsync() {
  return LaunchDetachedWorker(&g_probe_c62_launching, ProbeC62WorkerMain,
                              "Video2CameraService[C62]");
}

bool EnsureVideo2CameraServiceStarted() {
  bool expected = false;
  if (!g_service_started.compare_exchange_strong(expected, true)) {
    return g_service_registered.load();
  }

  auto fail = [&](const char *msg) {
    LOGE("%s", msg);
    g_service_started.store(false);
    return false;
  };

  LOGI("Video2CameraService: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    return fail("Video2CameraService: failed to load binder runtime API");
  }
  if (!EnsureServiceClass()) {
    return fail("Video2CameraService: failed to define service class");
  }

  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    return fail("Video2CameraService: AIBinder_new failed");
  }

  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();

  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Video2CameraService: add_service failed status=%d",
             add_status);
    return fail(buf);
  }

  g_service_registered.store(true);
  LOGI("Video2CameraService registered as %s", kVideo2CameraServiceName);
  return true;
}

namespace {
void *ServiceThreadMain(void *) {
  EnsureVideo2CameraServiceStarted();
  g_service_launching.store(false);
  return nullptr;
}
}  // namespace

void StartVideo2CameraServiceAsync() {
  if (g_service_registered.load()) return;

  bool expected = false;
  if (!g_service_launching.compare_exchange_strong(expected, true)) return;

  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ServiceThreadMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService: pthread_create failed rc=%d", rc);
    g_service_launching.store(false);
    return;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService: async startup requested");
}

}  // namespace awesomecam
