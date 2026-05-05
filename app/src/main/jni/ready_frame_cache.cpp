#include "ready_frame_cache.h"

#include <android/log.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "libyuv_runtime.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

constexpr size_t kMaxReadyTargets = 8;
constexpr int kLibYuvFilterBilinear = 2;

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
std::atomic<uint64_t> g_publish_count{0};

uint64_t now_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

double ns_to_ms(uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
}

size_t chroma_width_for(int width) { return static_cast<size_t>((width + 1) / 2); }
size_t chroma_height_for(int height) { return static_cast<size_t>((height + 1) / 2); }

size_t i420_size_for(int width, int height) {
  if (width <= 0 || height <= 0) return 0;
  const size_t y_size = static_cast<size_t>(width) * height;
  return y_size + 2 * chroma_width_for(width) * chroma_height_for(height);
}

bool same_key(const TargetKey &key, int32_t width, int32_t height) {
  return key.width == width && key.height == height;
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
  out_w = std::max(2, out_w);
  out_h = std::max(2, out_h);

  *crop_x = out_x;
  *crop_y = out_y;
  *crop_w = out_w;
  *crop_h = out_h;
}

void scale_plane_crop_nn(const uint8_t *src, int src_stride, int crop_x, int crop_y,
                         int crop_w, int crop_h, uint8_t *dst, int dst_stride,
                         int dst_w, int dst_h) {
  for (int y = 0; y < dst_h; ++y) {
    const int src_y = crop_y + (y * crop_h) / std::max(1, dst_h);
    const uint8_t *src_row = src + static_cast<size_t>(src_y) * src_stride;
    uint8_t *dst_row = dst + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < dst_w; ++x) {
      const int src_x = crop_x + (x * crop_w) / std::max(1, dst_w);
      dst_row[x] = src_row[src_x];
    }
  }
}

bool build_ready_scaled_i420(int src_width, int src_height,
                             const std::vector<uint8_t> &src_i420, int dst_width,
                             int dst_height, std::vector<uint8_t> *dst_i420) {
  if (dst_i420 == nullptr || src_width <= 0 || src_height <= 0 || dst_width <= 0 ||
      dst_height <= 0) {
    return false;
  }
  const size_t src_size = i420_size_for(src_width, src_height);
  const size_t dst_size = i420_size_for(dst_width, dst_height);
  if (src_i420.size() < src_size || dst_size == 0) return false;

  dst_i420->resize(dst_size);
  if (src_width == dst_width && src_height == dst_height) {
    memcpy(dst_i420->data(), src_i420.data(), dst_size);
    return true;
  }

  const size_t src_y_size = static_cast<size_t>(src_width) * src_height;
  const size_t src_chroma_width = chroma_width_for(src_width);
  const size_t src_chroma_height = chroma_height_for(src_height);
  const size_t src_chroma_size = src_chroma_width * src_chroma_height;

  const uint8_t *src_y_base = src_i420.data();
  const uint8_t *src_u_base = src_y_base + src_y_size;
  const uint8_t *src_v_base = src_u_base + src_chroma_size;

  const size_t dst_y_size = static_cast<size_t>(dst_width) * dst_height;
  const size_t dst_chroma_width = chroma_width_for(dst_width);
  const size_t dst_chroma_height = chroma_height_for(dst_height);
  uint8_t *dst_y = dst_i420->data();
  uint8_t *dst_u = dst_y + dst_y_size;
  uint8_t *dst_v = dst_u + dst_chroma_width * dst_chroma_height;

  int crop_x = 0;
  int crop_y = 0;
  int crop_w = src_width;
  int crop_h = src_height;
  compute_center_crop(src_width, src_height, dst_width, dst_height, &crop_x, &crop_y,
                      &crop_w, &crop_h);

  const uint8_t *src_y = src_y_base + static_cast<size_t>(crop_y) * src_width + crop_x;
  const uint8_t *src_u = src_u_base + static_cast<size_t>(crop_y / 2) * src_chroma_width +
                         (crop_x / 2);
  const uint8_t *src_v = src_v_base + static_cast<size_t>(crop_y / 2) * src_chroma_width +
                         (crop_x / 2);

  if (LibYuvI420Scale(src_y, src_width, src_u, static_cast<int>(src_chroma_width),
                      src_v, static_cast<int>(src_chroma_width), crop_w, crop_h,
                      dst_y, dst_width, dst_u, static_cast<int>(dst_chroma_width),
                      dst_v, static_cast<int>(dst_chroma_width), dst_width,
                      dst_height, kLibYuvFilterBilinear)) {
    return true;
  }

  scale_plane_crop_nn(src_y_base, src_width, crop_x, crop_y, crop_w, crop_h, dst_y,
                      dst_width, dst_width, dst_height);
  scale_plane_crop_nn(src_u_base, static_cast<int>(src_chroma_width), crop_x / 2,
                      crop_y / 2, crop_w / 2, crop_h / 2, dst_u,
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  scale_plane_crop_nn(src_v_base, static_cast<int>(src_chroma_width), crop_x / 2,
                      crop_y / 2, crop_w / 2, crop_h / 2, dst_v,
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  return true;
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

}  // namespace

void RegisterReadyFrameTarget(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) return;
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

void PublishReadyI420Source(int32_t width, int32_t height,
                            const std::vector<uint8_t> &bytes,
                            uint64_t generation) {
  const uint64_t perf_start_ns = now_ns();
  const size_t src_size = i420_size_for(width, height);
  if (src_size == 0 || bytes.size() < src_size) return;

  std::vector<TargetKey> targets;
  {
    std::lock_guard<std::mutex> lock(g_ready_mutex);
    targets = g_targets;
  }
  if (targets.empty()) return;

  uint64_t built_count = 0;
  for (const auto &target : targets) {
    auto frame = std::make_shared<ReadyI420Frame>();
    frame->width = target.width;
    frame->height = target.height;
    frame->generation = generation;
    if (!build_ready_scaled_i420(width, height, bytes, target.width, target.height,
                                 &frame->bytes)) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(g_ready_mutex);
      store_ready_frame_locked(std::move(frame));
    }
    built_count += 1;
  }

  const uint64_t count = g_publish_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 5 || (count % 120) == 0) {
    LOGI("ReadyFrameCache publish gen=%llu src=%dx%d targets=%zu built=%llu ms=%.3f",
         static_cast<unsigned long long>(generation), width, height, targets.size(),
         static_cast<unsigned long long>(built_count),
         ns_to_ms(now_ns() - perf_start_ns));
  }
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
}

}  // namespace awesomecam
