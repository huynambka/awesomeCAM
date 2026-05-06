#include "libyuv_runtime.h"

#include <android/log.h>
#include <dlfcn.h>
#include <mutex>

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

using Android420ToI420Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                                   const uint8_t *src_u, int src_stride_u,
                                   const uint8_t *src_v, int src_stride_v,
                                   int src_pixel_stride_uv,
                                   uint8_t *dst_y, int dst_stride_y,
                                   uint8_t *dst_u, int dst_stride_u,
                                   uint8_t *dst_v, int dst_stride_v,
                                   int width, int height);
using I420ScaleFn = int (*)(const uint8_t *src_y, int src_stride_y,
                            const uint8_t *src_u, int src_stride_u,
                            const uint8_t *src_v, int src_stride_v,
                            int src_width, int src_height,
                            uint8_t *dst_y, int dst_stride_y,
                            uint8_t *dst_u, int dst_stride_u,
                            uint8_t *dst_v, int dst_stride_v,
                            int dst_width, int dst_height,
                            int filtering);
using I420CopyFn = int (*)(const uint8_t *src_y, int src_stride_y,
                           const uint8_t *src_u, int src_stride_u,
                           const uint8_t *src_v, int src_stride_v,
                           uint8_t *dst_y, int dst_stride_y,
                           uint8_t *dst_u, int dst_stride_u,
                           uint8_t *dst_v, int dst_stride_v,
                           int width, int height);
using I420ToNV12Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                             const uint8_t *src_u, int src_stride_u,
                             const uint8_t *src_v, int src_stride_v,
                             uint8_t *dst_y, int dst_stride_y,
                             uint8_t *dst_uv, int dst_stride_uv,
                             int width, int height);
using I420ToNV21Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                             const uint8_t *src_u, int src_stride_u,
                             const uint8_t *src_v, int src_stride_v,
                             uint8_t *dst_y, int dst_stride_y,
                             uint8_t *dst_vu, int dst_stride_vu,
                             int width, int height);
using NV12ToI420Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                             const uint8_t *src_uv, int src_stride_uv,
                             uint8_t *dst_y, int dst_stride_y,
                             uint8_t *dst_u, int dst_stride_u,
                             uint8_t *dst_v, int dst_stride_v,
                             int width, int height);
using NV21ToI420Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                             const uint8_t *src_vu, int src_stride_vu,
                             uint8_t *dst_y, int dst_stride_y,
                             uint8_t *dst_u, int dst_stride_u,
                             uint8_t *dst_v, int dst_stride_v,
                             int width, int height);

struct LibYuvRuntime {
  void *handle = nullptr;
  Android420ToI420Fn android420_to_i420 = nullptr;
  I420ScaleFn i420_scale = nullptr;
  I420CopyFn i420_copy = nullptr;
  I420ToNV12Fn i420_to_nv12 = nullptr;
  I420ToNV21Fn i420_to_nv21 = nullptr;
  NV12ToI420Fn nv12_to_i420 = nullptr;
  NV21ToI420Fn nv21_to_i420 = nullptr;
  bool ok = false;
};

std::once_flag g_init_once;
LibYuvRuntime g_runtime;

void InitLibYuvRuntime() {
  constexpr const char *kCandidates[] = {
      "libyuv.so",
      "/system/lib64/libyuv.so",
      "/apex/com.google.pixel.camera.hal/lib64/libyuv.so",
      "/apex/com.android.media.swcodec/lib64/libyuv.so",
  };

  for (const char *path : kCandidates) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) continue;

    auto android420_to_i420 = reinterpret_cast<Android420ToI420Fn>(
        dlsym(handle, "Android420ToI420"));
    auto i420_scale = reinterpret_cast<I420ScaleFn>(dlsym(handle, "I420Scale"));
    auto i420_copy = reinterpret_cast<I420CopyFn>(dlsym(handle, "I420Copy"));
    auto i420_to_nv12 = reinterpret_cast<I420ToNV12Fn>(dlsym(handle, "I420ToNV12"));
    auto i420_to_nv21 = reinterpret_cast<I420ToNV21Fn>(dlsym(handle, "I420ToNV21"));
    auto nv12_to_i420 = reinterpret_cast<NV12ToI420Fn>(dlsym(handle, "NV12ToI420"));
    auto nv21_to_i420 = reinterpret_cast<NV21ToI420Fn>(dlsym(handle, "NV21ToI420"));
    if (android420_to_i420 != nullptr && i420_scale != nullptr) {
      g_runtime.handle = handle;
      g_runtime.android420_to_i420 = android420_to_i420;
      g_runtime.i420_scale = i420_scale;
      g_runtime.i420_copy = i420_copy;
      g_runtime.i420_to_nv12 = i420_to_nv12;
      g_runtime.i420_to_nv21 = i420_to_nv21;
      g_runtime.nv12_to_i420 = nv12_to_i420;
      g_runtime.nv21_to_i420 = nv21_to_i420;
      g_runtime.ok = true;
      LOGI("libyuv runtime loaded from %s copy=%d i420ToNv12=%d i420ToNv21=%d nv12ToI420=%d nv21ToI420=%d", path,
           i420_copy != nullptr ? 1 : 0, i420_to_nv12 != nullptr ? 1 : 0,
           i420_to_nv21 != nullptr ? 1 : 0, nv12_to_i420 != nullptr ? 1 : 0,
           nv21_to_i420 != nullptr ? 1 : 0);
      return;
    }

    dlclose(handle);
  }

  LOGW("libyuv runtime unavailable; using manual YUV copy/scale fallback");
}

const LibYuvRuntime &Runtime() {
  std::call_once(g_init_once, InitLibYuvRuntime);
  return g_runtime;
}

}  // namespace

bool LibYuvAvailable() { return Runtime().ok; }

bool LibYuvAndroid420ToI420(const uint8_t *src_y, int src_stride_y,
                            const uint8_t *src_u, int src_stride_u,
                            const uint8_t *src_v, int src_stride_v,
                            int src_pixel_stride_uv,
                            uint8_t *dst_y, int dst_stride_y,
                            uint8_t *dst_u, int dst_stride_u,
                            uint8_t *dst_v, int dst_stride_v,
                            int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.android420_to_i420 == nullptr) return false;
  return rt.android420_to_i420(src_y, src_stride_y, src_u, src_stride_u,
                               src_v, src_stride_v, src_pixel_stride_uv,
                               dst_y, dst_stride_y, dst_u, dst_stride_u,
                               dst_v, dst_stride_v, width, height) == 0;
}

bool LibYuvI420Scale(const uint8_t *src_y, int src_stride_y,
                     const uint8_t *src_u, int src_stride_u,
                     const uint8_t *src_v, int src_stride_v,
                     int src_width, int src_height,
                     uint8_t *dst_y, int dst_stride_y,
                     uint8_t *dst_u, int dst_stride_u,
                     uint8_t *dst_v, int dst_stride_v,
                     int dst_width, int dst_height,
                     int filtering) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.i420_scale == nullptr) return false;
  return rt.i420_scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                       src_stride_v, src_width, src_height, dst_y,
                       dst_stride_y, dst_u, dst_stride_u, dst_v,
                       dst_stride_v, dst_width, dst_height, filtering) == 0;
}

bool LibYuvI420Copy(const uint8_t *src_y, int src_stride_y,
                    const uint8_t *src_u, int src_stride_u,
                    const uint8_t *src_v, int src_stride_v,
                    uint8_t *dst_y, int dst_stride_y,
                    uint8_t *dst_u, int dst_stride_u,
                    uint8_t *dst_v, int dst_stride_v,
                    int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.i420_copy == nullptr) return false;
  return rt.i420_copy(src_y, src_stride_y, src_u, src_stride_u, src_v,
                      src_stride_v, dst_y, dst_stride_y, dst_u, dst_stride_u,
                      dst_v, dst_stride_v, width, height) == 0;
}

bool LibYuvNV12ToI420(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_uv, int src_stride_uv,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_u, int dst_stride_u,
                      uint8_t *dst_v, int dst_stride_v,
                      int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.nv12_to_i420 == nullptr) return false;
  return rt.nv12_to_i420(src_y, src_stride_y, src_uv, src_stride_uv,
                         dst_y, dst_stride_y, dst_u, dst_stride_u,
                         dst_v, dst_stride_v, width, height) == 0;
}

bool LibYuvNV21ToI420(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_vu, int src_stride_vu,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_u, int dst_stride_u,
                      uint8_t *dst_v, int dst_stride_v,
                      int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.nv21_to_i420 == nullptr) return false;
  return rt.nv21_to_i420(src_y, src_stride_y, src_vu, src_stride_vu,
                         dst_y, dst_stride_y, dst_u, dst_stride_u,
                         dst_v, dst_stride_v, width, height) == 0;
}

bool LibYuvI420ToNV12(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_u, int src_stride_u,
                      const uint8_t *src_v, int src_stride_v,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_uv, int dst_stride_uv,
                      int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.i420_to_nv12 == nullptr) return false;
  return rt.i420_to_nv12(src_y, src_stride_y, src_u, src_stride_u, src_v,
                         src_stride_v, dst_y, dst_stride_y, dst_uv,
                         dst_stride_uv, width, height) == 0;
}

bool LibYuvI420ToNV21(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_u, int src_stride_u,
                      const uint8_t *src_v, int src_stride_v,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_vu, int dst_stride_vu,
                      int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.i420_to_nv21 == nullptr) return false;
  return rt.i420_to_nv21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                         src_stride_v, dst_y, dst_stride_y, dst_vu,
                         dst_stride_vu, width, height) == 0;
}

}  // namespace awesomecam
