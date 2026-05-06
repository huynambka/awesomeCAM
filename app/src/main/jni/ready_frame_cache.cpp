#include "ready_frame_cache.h"

#include <android/log.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

struct SwsContext;
using FnSwsGetContext = SwsContext *(*)(int, int, int, int, int, int, int, void *, void *, const double *);
using FnSwsScale = int (*)(SwsContext *, const uint8_t *const[], const int[], int, int, uint8_t *const[], const int[]);
using FnSwsFreeContext = void (*)(SwsContext *);


#include "video2camera_ipc.h"
#include "video2camera_service.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

constexpr size_t kMaxReadyTargets = 8;
constexpr int kSwsFlags = 1;  // SWS_FAST_BILINEAR.
constexpr int kAvPixFmtYuv420p = 0;

struct TargetKey {
  int32_t width = 0;
  int32_t height = 0;
};

struct ReadySlot {
  TargetKey key;
  std::shared_ptr<const ReadyI420Frame> frame;
};

std::mutex g_ready_mutex;
std::vector<TargetKey> g_targets;
std::vector<ReadySlot> g_slots;
uint64_t g_last_source_generation = 0;
std::atomic<uint64_t> g_publish_count{0};
std::atomic<bool> g_worker_started{false};

struct SwsApi {
  void *handle = nullptr;
  FnSwsGetContext get_context = nullptr;
  FnSwsScale scale = nullptr;
  FnSwsFreeContext free_context = nullptr;
};

SwsApi g_sws_api;
std::mutex g_sws_api_mutex;

uint64_t now_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

double ns_to_ms(uint64_t ns) { return static_cast<double>(ns) / 1000000.0; }

size_t chroma_width_for(int width) { return static_cast<size_t>((width + 1) / 2); }
size_t chroma_height_for(int height) { return static_cast<size_t>((height + 1) / 2); }

bool same_key(const TargetKey &key, int32_t width, int32_t height) {
  return key.width == width && key.height == height;
}

bool LoadSwsApi() {
  std::lock_guard<std::mutex> lock(g_sws_api_mutex);
  if (g_sws_api.handle != nullptr) {
    return g_sws_api.get_context != nullptr && g_sws_api.scale != nullptr &&
           g_sws_api.free_context != nullptr;
  }
  g_sws_api.handle = dlopen("/data/camera/libffmpeg.so", RTLD_NOW | RTLD_LOCAL);
  if (g_sws_api.handle == nullptr) {
    g_sws_api.handle = dlopen("libffmpeg.so", RTLD_NOW | RTLD_LOCAL);
  }
  if (g_sws_api.handle == nullptr) {
    LOGE("ReadyFrameCache: dlopen libffmpeg.so failed: %s", dlerror() ? dlerror() : "unknown");
    return false;
  }
  g_sws_api.get_context = reinterpret_cast<FnSwsGetContext>(dlsym(g_sws_api.handle, "sws_getContext"));
  g_sws_api.scale = reinterpret_cast<FnSwsScale>(dlsym(g_sws_api.handle, "sws_scale"));
  g_sws_api.free_context = reinterpret_cast<FnSwsFreeContext>(dlsym(g_sws_api.handle, "sws_freeContext"));
  const bool ok = g_sws_api.get_context != nullptr && g_sws_api.scale != nullptr &&
                  g_sws_api.free_context != nullptr;
  if (!ok) {
    LOGE("ReadyFrameCache: missing swscale symbols get=%p scale=%p free=%p",
         reinterpret_cast<void *>(g_sws_api.get_context), reinterpret_cast<void *>(g_sws_api.scale),
         reinterpret_cast<void *>(g_sws_api.free_context));
  }
  return ok;
}

void compute_center_crop(int src_w, int src_h, int dst_w, int dst_h, int *crop_x,
                         int *crop_y, int *crop_w, int *crop_h) {
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
  *crop_x = out_x;
  *crop_y = out_y;
  *crop_w = std::max(2, out_w);
  *crop_h = std::max(2, out_h);
}

bool build_ready_scaled_i420(int src_width, int src_height, const uint8_t *src_i420,
                             size_t src_size, int dst_width, int dst_height,
                             std::vector<uint8_t> *dst_i420) {
  if (src_width <= 0 || src_height <= 0 || src_i420 == nullptr || dst_width <= 0 ||
      dst_height <= 0 || dst_i420 == nullptr) {
    return false;
  }
  const size_t needed = I420FrameSize(src_width, src_height);
  const size_t dst_size = I420FrameSize(dst_width, dst_height);
  if (needed == 0 || src_size < needed || dst_size == 0) return false;

  dst_i420->resize(dst_size);
  if (src_width == dst_width && src_height == dst_height) {
    memcpy(dst_i420->data(), src_i420, dst_size);
    return true;
  }

  const int src_cw = static_cast<int>(chroma_width_for(src_width));
  const int src_ch = static_cast<int>(chroma_height_for(src_height));
  const size_t src_y_size = static_cast<size_t>(src_width) * src_height;
  const size_t src_c_size = static_cast<size_t>(src_cw) * src_ch;
  const uint8_t *src_y_base = src_i420;
  const uint8_t *src_u_base = src_y_base + src_y_size;
  const uint8_t *src_v_base = src_u_base + src_c_size;

  const int dst_cw = static_cast<int>(chroma_width_for(dst_width));
  const int dst_ch = static_cast<int>(chroma_height_for(dst_height));
  const size_t dst_y_size = static_cast<size_t>(dst_width) * dst_height;
  const size_t dst_c_size = static_cast<size_t>(dst_cw) * dst_ch;
  uint8_t *dst_y = dst_i420->data();
  uint8_t *dst_u = dst_y + dst_y_size;
  uint8_t *dst_v = dst_u + dst_c_size;

  int crop_x = 0;
  int crop_y = 0;
  int crop_w = src_width;
  int crop_h = src_height;
  compute_center_crop(src_width, src_height, dst_width, dst_height, &crop_x, &crop_y,
                      &crop_w, &crop_h);

  const uint8_t *src_y = src_y_base + static_cast<size_t>(crop_y) * src_width + crop_x;
  const uint8_t *src_u = src_u_base + static_cast<size_t>(crop_y / 2) * src_cw + crop_x / 2;
  const uint8_t *src_v = src_v_base + static_cast<size_t>(crop_y / 2) * src_cw + crop_x / 2;

  const uint8_t *src_data[4] = {src_y, src_u, src_v, nullptr};
  const int src_linesize[4] = {src_width, src_cw, src_cw, 0};
  uint8_t *dst_data[4] = {dst_y, dst_u, dst_v, nullptr};
  const int dst_linesize[4] = {dst_width, dst_cw, dst_cw, 0};

  if (!LoadSwsApi()) return false;
  SwsContext *ctx = g_sws_api.get_context(crop_w, crop_h, kAvPixFmtYuv420p,
                                          dst_width, dst_height, kAvPixFmtYuv420p,
                                          kSwsFlags, nullptr, nullptr, nullptr);
  if (ctx == nullptr) {
    LOGE("ReadyFrameCache sws_getContext failed src=%dx%d crop=%dx%d dst=%dx%d",
         src_width, src_height, crop_w, crop_h, dst_width, dst_height);
    return false;
  }
  const int scaled = g_sws_api.scale(ctx, src_data, src_linesize, 0, crop_h, dst_data,
                                     dst_linesize);
  g_sws_api.free_context(ctx);
  return scaled == dst_height;
}

void store_ready_frame_locked(std::shared_ptr<const ReadyI420Frame> frame) {
  if (!frame) return;
  for (auto &slot : g_slots) {
    if (same_key(slot.key, frame->width, frame->height)) {
      slot.frame = std::move(frame);
      return;
    }
  }
  g_slots.push_back(ReadySlot{TargetKey{frame->width, frame->height}, std::move(frame)});
}

bool cache_has_fresh_frame_locked(const TargetKey &target, uint64_t generation) {
  for (const auto &slot : g_slots) {
    if (same_key(slot.key, target.width, target.height) && slot.frame &&
        slot.frame->generation == generation) {
      return true;
    }
  }
  return false;
}

void BuildAndStoreLatestSource() {
  SharedMemoryFrameView source{};
  if (!CopyLatestSourceFrame(&source) || source.bytes == nullptr || source.size == 0) {
    usleep(10000);
    return;
  }

  std::vector<TargetKey> targets;
  {
    std::lock_guard<std::mutex> lock(g_ready_mutex);
    if (g_targets.empty()) return;
    bool needs_build = source.generation != g_last_source_generation;
    if (!needs_build) {
      for (const auto &target : g_targets) {
        if (!cache_has_fresh_frame_locked(target, source.generation)) {
          needs_build = true;
          break;
        }
      }
    }
    if (!needs_build) return;
    targets = g_targets;
  }

  const uint64_t perf_start_ns = now_ns();
  std::vector<uint8_t> source_copy(source.bytes, source.bytes + source.size);
  std::vector<std::shared_ptr<const ReadyI420Frame>> built_frames;
  built_frames.reserve(targets.size());
  for (const auto &target : targets) {
    auto frame = std::make_shared<ReadyI420Frame>();
    frame->width = target.width;
    frame->height = target.height;
    frame->generation = source.generation;
    frame->pts_us = source.pts_us;
    if (!build_ready_scaled_i420(source.width, source.height, source_copy.data(),
                                 source_copy.size(), target.width, target.height,
                                 &frame->bytes)) {
      continue;
    }
    built_frames.push_back(std::move(frame));
  }

  {
    std::lock_guard<std::mutex> lock(g_ready_mutex);
    for (auto &frame : built_frames) store_ready_frame_locked(std::move(frame));
    g_last_source_generation = source.generation;
  }
  const uint64_t count = g_publish_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 10 || (count % 120) == 0) {
    LOGI("ReadyFrameCache built sourceGen=%llu src=%dx%d targets=%zu built=%zu ms=%.3f",
         static_cast<unsigned long long>(source.generation), source.width, source.height,
         targets.size(), built_frames.size(), ns_to_ms(now_ns() - perf_start_ns));
  }
}

void *ReadyWorkerMain(void *) {
  LOGI("ReadyFrameCache worker start");
  for (;;) {
    BuildAndStoreLatestSource();
    usleep(5000);
  }
  return nullptr;
}

void EnsureWorkerStarted() {
  bool expected = false;
  if (!g_worker_started.compare_exchange_strong(expected, true)) return;
  pthread_t thread{};
  const int rc = pthread_create(&thread, nullptr, ReadyWorkerMain, nullptr);
  if (rc != 0) {
    g_worker_started.store(false);
    LOGE("ReadyFrameCache worker pthread_create failed rc=%d", rc);
    return;
  }
  pthread_detach(thread);
}


}  // namespace

void RegisterReadyFrameTarget(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) return;
  EnsureWorkerStarted();
  std::lock_guard<std::mutex> lock(g_ready_mutex);
  for (const auto &target : g_targets) {
    if (same_key(target, width, height)) return;
  }
  if (g_targets.size() >= kMaxReadyTargets) {
    LOGW("ReadyFrameCache target full; ignore %dx%d", width, height);
    return;
  }
  g_targets.push_back(TargetKey{width, height});
  LOGI("ReadyFrameCache registered target %dx%d count=%zu", width, height,
       g_targets.size());
}

std::shared_ptr<const ReadyI420Frame> GetReadyI420Frame(int32_t width,
                                                        int32_t height) {
  if (width <= 0 || height <= 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_ready_mutex);
  for (const auto &slot : g_slots) {
    if (same_key(slot.key, width, height)) return slot.frame;
  }
  return nullptr;
}

void ClearReadyFrameCache() {
  std::lock_guard<std::mutex> lock(g_ready_mutex);
  g_slots.clear();
  g_targets.clear();
  g_last_source_generation = 0;
}

}  // namespace awesomecam
