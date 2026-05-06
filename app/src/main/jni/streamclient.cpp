#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <jni.h>

#include <mutex>

#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

std::mutex g_mutex;
BinderRuntimeApi g_binder_runtime;
AIBinder_Class *g_client_class = nullptr;
AIBinder *g_remote = nullptr;

void *ClientOnCreate(void *) { return nullptr; }
void ClientOnDestroy(void *) {}
binder_status_t ClientOnTransact(AIBinder *, transaction_code_t, const AParcel *, AParcel *) {
  return STATUS_UNKNOWN_TRANSACTION;
}

bool ConnectLocked(bool wait) {
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("VideoBridge: failed to load binder runtime");
    return false;
  }
  if (g_client_class == nullptr) {
    g_client_class = g_binder_runtime.binder_class_define(
        kVideo2CameraDescriptor, ClientOnCreate, ClientOnDestroy, ClientOnTransact);
    if (g_client_class == nullptr) {
      LOGE("VideoBridge: failed to define binder client class");
      return false;
    }
  }
  if (g_remote != nullptr && g_binder_runtime.binder_is_alive(g_remote)) return true;
  g_remote = wait ? g_binder_runtime.wait_for_service(kVideo2CameraServiceName)
                  : g_binder_runtime.check_service(kVideo2CameraServiceName);
  if (g_remote != nullptr && !g_binder_runtime.binder_associate_class(g_remote, g_client_class)) {
    LOGE("VideoBridge: AIBinder_associateClass failed remote=%p", g_remote);
    g_remote = nullptr;
  }
  LOGI("VideoBridge: service %s remote=%p", wait ? "wait" : "check", g_remote);
  return g_remote != nullptr;
}

bool TransactClearLocked() {
  if (!ConnectLocked(false)) return false;
  AParcel *in = nullptr;
  AParcel *out = nullptr;
  binder_status_t status = g_binder_runtime.binder_prepare_transaction(g_remote, &in);
  if (status == STATUS_OK) {
    status = g_binder_runtime.binder_transact(g_remote, kTxnClear, &in, &out, 0);
  }
  int32_t ack = 0;
  if (status == STATUS_OK && out != nullptr) g_binder_runtime.parcel_read_int32(out, &ack);
  if (in != nullptr) g_binder_runtime.parcel_delete(in);
  if (out != nullptr) g_binder_runtime.parcel_delete(out);
  LOGI("VideoBridge: clear status=%d ack=%d", status, ack);
  return status == STATUS_OK && ack == 1;
}

}  // namespace
}  // namespace awesomecam

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeConnect(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_mutex);
  return awesomecam::ConnectLocked(true) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeStartFeed(JNIEnv *, jclass, jstring) {
  // The ref_vcam-style path starts /data/camera/awesomecam_player as root from Kotlin.
  // Keep this JNI method as a compatibility no-op for older UI/service code paths.
  LOGI("VideoBridge: nativeStartFeed is handled by awesomecam_player root process");
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeStopFeed(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_mutex);
  (void)awesomecam::TransactClearLocked();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeIsFeedRunning(JNIEnv *, jclass) {
  return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeClear(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_mutex);
  return awesomecam::TransactClearLocked() ? JNI_TRUE : JNI_FALSE;
}
