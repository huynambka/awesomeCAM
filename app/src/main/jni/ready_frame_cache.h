#pragma once

#include <stdint.h>

#include <memory>
#include <vector>

namespace awesomecam {

struct ReadyI420Frame {
  int32_t width = 0;
  int32_t height = 0;
  uint64_t generation = 0;
  int64_t pts_us = 0;
  std::vector<uint8_t> bytes;
};

void RegisterReadyFrameTarget(int32_t width, int32_t height);
std::shared_ptr<const ReadyI420Frame> GetReadyI420Frame(int32_t width,
                                                        int32_t height);
void ClearReadyFrameCache();

}  // namespace awesomecam
