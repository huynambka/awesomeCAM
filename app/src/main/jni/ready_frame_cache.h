#pragma once

#include <stdint.h>

#include <memory>
#include <vector>

namespace awesomecam {

enum ReadyFrameLayout : int32_t {
  kReadyFrameLayoutI420 = 1,
  kReadyFrameLayoutNV12 = 2,
  kReadyFrameLayoutNV21 = 3,
};

struct ReadyI420Frame {
  int32_t width = 0;
  int32_t height = 0;
  int32_t layout = kReadyFrameLayoutI420;
  int32_t y_stride = 0;
  int32_t c_stride = 0;
  uint64_t generation = 0;
  int64_t pts_us = 0;
  std::vector<uint8_t> bytes;
};

void RegisterReadyFrameTarget(int32_t width, int32_t height);
void RegisterReadyFrameOutputLayout(int32_t width, int32_t height,
                                    ReadyFrameLayout layout);
std::shared_ptr<const ReadyI420Frame> GetReadyI420Frame(int32_t width,
                                                        int32_t height);
std::shared_ptr<const ReadyI420Frame> GetReadyFrameForLayout(
    int32_t width, int32_t height, ReadyFrameLayout layout);
void ClearReadyFrameCache();

}  // namespace awesomecam
