#include "video2camera_service.h"
#include "video2camera_player.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ready_frame_cache.h"
#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

struct IncomingByteArray {
  std::vector<int8_t> data;
};

struct SharedFrameState {
  std::mutex mutex;
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = kFrameFormatUnknown;
  std::vector<uint8_t> bytes;
  uint64_t generation = 0;
  uint64_t push_count = 0;
};

SharedFrameState g_shared_frame;
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

using FnProcessStateStartThreadPool = void (*)(void *);
using FnIPCThreadStateSelf = void *(*)();
using FnIPCThreadStateJoinThreadPool = void (*)(void *, bool);

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
    LOGE("ClassicBinder: dlopen(libbinder.so) failed: %s", dlerror() ? dlerror() : "unknown");
    return false;
  }
  api->sym_process_state_self = dlsym(api->handle, "_ZN7android12ProcessState4selfEv");
  api->sym_process_state_start_thread_pool = dlsym(api->handle, "_ZN7android12ProcessState15startThreadPoolEv");
  api->sym_ipc_thread_state_self = dlsym(api->handle, "_ZN7android14IPCThreadState4selfEv");
  api->sym_ipc_thread_state_join_thread_pool = dlsym(api->handle, "_ZN7android14IPCThreadState14joinThreadPoolEb");
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


bool ByteArrayAllocator(void *arrayData, int32_t length, int8_t **outBuffer) {
  if (arrayData == nullptr || outBuffer == nullptr) return false;
  auto *incoming = static_cast<IncomingByteArray *>(arrayData);
  if (length < 0) {
    incoming->data.clear();
    *outBuffer = nullptr;
    return true;
  }
  incoming->data.resize(static_cast<size_t>(length));
  *outBuffer = incoming->data.empty() ? nullptr : incoming->data.data();
  return true;
}

void *OnCreate(void *) { return nullptr; }
void OnDestroy(void *) {}

binder_status_t OnTransact(AIBinder *, transaction_code_t code, const AParcel *in,
                           AParcel *out) {
  switch (code) {
    case kTxnSetFrame: {
      int32_t width = 0;
      int32_t height = 0;
      int32_t format = 0;
      binder_status_t status = g_binder_runtime.parcel_read_int32(in, &width);
      if (status != STATUS_OK) return status;
      status = g_binder_runtime.parcel_read_int32(in, &height);
      if (status != STATUS_OK) return status;
      status = g_binder_runtime.parcel_read_int32(in, &format);
      if (status != STATUS_OK) return status;

      IncomingByteArray incoming;
      status = g_binder_runtime.parcel_read_byte_array(in, &incoming, ByteArrayAllocator);
      if (status != STATUS_OK) return status;

      if (width <= 0 || height <= 0 || format != kFrameFormatI420) {
        LOGW("Video2CameraService reject frame width=%d height=%d format=%d size=%zu",
             width, height, format, incoming.data.size());
        return STATUS_BAD_VALUE;
      }

      const size_t expected = static_cast<size_t>(width) * height * 3 / 2;
      if (incoming.data.size() < expected) {
        LOGW("Video2CameraService frame too small size=%zu expected=%zu", incoming.data.size(),
             expected);
        return STATUS_BAD_VALUE;
      }

      StopNativePlayback();
      PublishDecodedI420Frame(
          width, height,
          std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(incoming.data.data()),
                               reinterpret_cast<const uint8_t *>(incoming.data.data()) + expected));
      if (out != nullptr) {
        BinderFrameState state{};
        if (PeekLatestBinderFrameState(&state)) {
          g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(state.generation));
        }
      }
      return STATUS_OK;
    }
    case kTxnClearFrame: {
      StopNativePlayback();
      ClearLatestBinderFrame();
      if (out != nullptr) {
        BinderFrameState state{};
        if (PeekLatestBinderFrameState(&state)) {
          g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(state.generation));
        } else {
          g_binder_runtime.parcel_write_int32(out, 0);
        }
      }
      LOGI("Video2CameraService cleared cached frame");
      return STATUS_OK;
    }
    case kTxnPlayFile: {
      IncomingByteArray incoming;
      binder_status_t status = g_binder_runtime.parcel_read_byte_array(in, &incoming, ByteArrayAllocator);
      if (status != STATUS_OK) return status;
      std::string path(incoming.data.begin(), incoming.data.end());
      if (path.empty()) return STATUS_BAD_VALUE;
      std::string error;
      if (!StartNativePlayback(path, &error)) {
        LOGE("Video2CameraService play failed for %s: %s", path.c_str(), error.c_str());
        return STATUS_UNKNOWN_ERROR;
      }
      LOGI("Video2CameraService native playback started path=%s", path.c_str());
      if (out != nullptr) {
        g_binder_runtime.parcel_write_int32(out, 1);
      }
      return STATUS_OK;
    }
    case kTxnStopPlayback: {
      StopNativePlayback();
      if (out != nullptr) {
        g_binder_runtime.parcel_write_int32(out, 1);
      }
      LOGI("Video2CameraService native playback stopped");
      return STATUS_OK;
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

void PublishDecodedI420Frame(int32_t width, int32_t height, std::vector<uint8_t> &&bytes) {
  if (width <= 0 || height <= 0 || bytes.empty()) return;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
    generation = g_shared_frame.generation + 1;
  }
  PublishReadyI420Source(width, height, bytes, generation);

  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  g_shared_frame.width = width;
  g_shared_frame.height = height;
  g_shared_frame.format = kFrameFormatI420;
  g_shared_frame.bytes = std::move(bytes);
  g_shared_frame.generation = generation;
  g_shared_frame.push_count += 1;
  if (g_shared_frame.push_count <= 5 || (g_shared_frame.push_count % 120) == 0) {
    LOGI("Video2CameraService publish decoded frame #%llu %dx%d bytes=%zu gen=%llu",
         static_cast<unsigned long long>(g_shared_frame.push_count), width, height,
         g_shared_frame.bytes.size(),
         static_cast<unsigned long long>(g_shared_frame.generation));
  }
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
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C2]: failed to load binder runtime API");
    return false;
  }
  LOGI("Video2CameraService[C2]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  LOGI("Video2CameraService[C2]: defining binder class only");
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C2]: failed to define service class");
    return false;
  }
  LOGI("Video2CameraService[C2]: binder class=%p", g_service_class);
  return true;
}

bool ProbeBinderNewOnly() {
  LOGI("Video2CameraService[C3]: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C3]: failed to load binder runtime API");
    return false;
  }
  LOGI("Video2CameraService[C3]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  LOGI("Video2CameraService[C3]: ensuring binder class");
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C3]: failed to define service class");
    return false;
  }
  LOGI("Video2CameraService[C3]: creating binder instance only");
  AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (binder == nullptr) {
    LOGE("Video2CameraService[C3]: AIBinder_new failed");
    return false;
  }
  LOGI("Video2CameraService[C3]: binder instance=%p", binder);
  return true;
}

bool ProbeBinderThreadPoolOnly() {
  LOGI("Video2CameraService[C4]: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C4]: failed to load binder runtime API");
    return false;
  }
  LOGI("Video2CameraService[C4]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  LOGI("Video2CameraService[C4]: ensuring binder class");
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C4]: failed to define service class");
    return false;
  }
  LOGI("Video2CameraService[C4]: creating binder instance");
  AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (binder == nullptr) {
    LOGE("Video2CameraService[C4]: AIBinder_new failed");
    return false;
  }
  LOGI("Video2CameraService[C4]: binder instance=%p", binder);
  LOGI("Video2CameraService[C4]: starting binder threadpool only");
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();
  LOGI("Video2CameraService[C4]: binder threadpool started");
  return true;
}

namespace {
void *ProbeC51WorkerMain(void *) {
  LOGI("Video2CameraService[C51]: worker start");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C51]: failed to load binder runtime API");
    g_probe_c51_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C51]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C51]: failed to define service class");
    g_probe_c51_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C51]: creating binder instance");
  AIBinder *binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (binder == nullptr) {
    LOGE("Video2CameraService[C51]: AIBinder_new failed");
    g_probe_c51_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C51]: binder instance=%p", binder);
  LOGI("Video2CameraService[C51]: starting binder threadpool from worker");
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();
  LOGI("Video2CameraService[C51]: binder threadpool started");
  g_probe_c51_launching.store(false);
  return nullptr;
}

void *ProbeC52WorkerMain(void *) {
  LOGI("Video2CameraService[C52]: worker start");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C52]: failed to load binder runtime API");
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C52]: binder runtime loaded handle=%p", g_binder_runtime.handle);
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C52]: failed to define service class");
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C52]: creating binder instance");
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    LOGE("Video2CameraService[C52]: AIBinder_new failed");
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C52]: binder instance=%p", g_service_binder);
  LOGI("Video2CameraService[C52]: starting binder threadpool from worker");
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();
  LOGI("Video2CameraService[C52]: binder threadpool started");
  LOGI("Video2CameraService[C52]: adding service %s", kVideo2CameraServiceName);
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    LOGE("Video2CameraService[C52]: add_service failed status=%d", add_status);
    g_probe_c52_launching.store(false);
    return nullptr;
  }
  g_service_registered.store(true);
  LOGI("Video2CameraService[C52]: registered as %s", kVideo2CameraServiceName);
  g_probe_c52_launching.store(false);
  return nullptr;
}
}  // namespace

bool ProbeBinderThreadPoolWorkerOnly() {
  bool expected = false;
  if (!g_probe_c51_launching.compare_exchange_strong(expected, true)) {
    LOGW("Video2CameraService[C51]: worker already launching");
    return true;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ProbeC51WorkerMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService[C51]: pthread_create failed rc=%d", rc);
    g_probe_c51_launching.store(false);
    return false;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService[C51]: worker launched");
  return true;
}

bool ProbeBinderFullServiceWorkerAsync() {
  bool expected = false;
  if (!g_probe_c52_launching.compare_exchange_strong(expected, true)) {
    LOGW("Video2CameraService[C52]: worker already launching");
    return true;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ProbeC52WorkerMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService[C52]: pthread_create failed rc=%d", rc);
    g_probe_c52_launching.store(false);
    return false;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService[C52]: worker launched");
  return true;
}

namespace {
void *ProbeC61WorkerMain(void *) {
  LOGI("Video2CameraService[C61]: worker start");
  if (!LoadClassicBinderRuntimeApi(&g_classic_binder_runtime)) {
    LOGE("Video2CameraService[C61]: failed to load classic binder runtime");
    g_probe_c61_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C61]: classic binder runtime loaded handle=%p",
       g_classic_binder_runtime.handle);
  StartClassicBinderThreadPool();
  g_probe_c61_launching.store(false);
  return nullptr;
}

void *ProbeC62WorkerMain(void *) {
  LOGI("Video2CameraService[C62]: worker start");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("Video2CameraService[C62]: failed to load binder runtime API");
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  if (!EnsureServiceClass()) {
    LOGE("Video2CameraService[C62]: failed to define service class");
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    LOGE("Video2CameraService[C62]: AIBinder_new failed");
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C62]: binder instance=%p", g_service_binder);
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    LOGE("Video2CameraService[C62]: add_service failed status=%d", add_status);
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  g_service_registered.store(true);
  LOGI("Video2CameraService[C62]: registered as %s", kVideo2CameraServiceName);
  if (!LoadClassicBinderRuntimeApi(&g_classic_binder_runtime)) {
    LOGE("Video2CameraService[C62]: failed to load classic binder runtime");
    g_probe_c62_launching.store(false);
    return nullptr;
  }
  LOGI("Video2CameraService[C62]: classic binder runtime loaded handle=%p",
       g_classic_binder_runtime.handle);
  StartClassicBinderThreadPool();
  g_probe_c62_launching.store(false);
  return nullptr;
}
}  // namespace

bool ProbeClassicThreadPoolWorkerOnly() {
  bool expected = false;
  if (!g_probe_c61_launching.compare_exchange_strong(expected, true)) {
    LOGW("Video2CameraService[C61]: worker already launching");
    return true;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ProbeC61WorkerMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService[C61]: pthread_create failed rc=%d", rc);
    g_probe_c61_launching.store(false);
    return false;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService[C61]: worker launched");
  return true;
}

bool ProbeClassicServiceWorkerAsync() {
  bool expected = false;
  if (!g_probe_c62_launching.compare_exchange_strong(expected, true)) {
    LOGW("Video2CameraService[C62]: worker already launching");
    return true;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ProbeC62WorkerMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService[C62]: pthread_create failed rc=%d", rc);
    g_probe_c62_launching.store(false);
    return false;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService[C62]: worker launched");
  return true;
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
  LOGI("Video2CameraService: defining binder class");
  if (!EnsureServiceClass()) {
    return fail("Video2CameraService: failed to define service class");
  }

  LOGI("Video2CameraService: creating binder instance");
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    return fail("Video2CameraService: AIBinder_new failed");
  }

  LOGI("Video2CameraService: starting binder threadpool");
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();

  LOGI("Video2CameraService: adding service %s", kVideo2CameraServiceName);
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Video2CameraService: add_service failed status=%d", add_status);
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
  if (!g_service_launching.compare_exchange_strong(expected, true)) {
    return;
  }

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

bool CopyLatestBinderFrame(BinderFrameCopy *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  if (g_shared_frame.width <= 0 || g_shared_frame.height <= 0 ||
      g_shared_frame.format != kFrameFormatI420 || g_shared_frame.bytes.empty()) {
    return false;
  }
  out->width = g_shared_frame.width;
  out->height = g_shared_frame.height;
  out->format = g_shared_frame.format;
  out->generation = g_shared_frame.generation;
  out->bytes = g_shared_frame.bytes;
  return true;
}

bool PeekLatestBinderFrameState(BinderFrameState *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  if (g_shared_frame.width <= 0 || g_shared_frame.height <= 0 ||
      g_shared_frame.format != kFrameFormatI420 || g_shared_frame.bytes.empty()) {
    return false;
  }
  out->width = g_shared_frame.width;
  out->height = g_shared_frame.height;
  out->format = g_shared_frame.format;
  out->generation = g_shared_frame.generation;
  return true;
}

void ClearLatestBinderFrame() {
  ClearReadyFrameCache();
  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  g_shared_frame.width = 0;
  g_shared_frame.height = 0;
  g_shared_frame.format = kFrameFormatUnknown;
  g_shared_frame.bytes.clear();
  g_shared_frame.generation += 1;
}

}  // namespace awesomecam
