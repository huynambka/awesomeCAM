#pragma once

#include <stddef.h>
#include <stdint.h>

namespace awesomecam {

static constexpr const char *kVideo2CameraServiceName = "Video2CameraService";
static constexpr const char *kVideo2CameraDescriptor =
    "com.namnh.awesomecam.Video2CameraService";

enum TransactionCode : uint32_t {
  kTxnGetTargets = 1,
  kTxnRegisterSourceMemory = 2,
  kTxnUnregisterSourceMemory = 3,
  kTxnClear = 4,
  kTxnGetSourceStatus = 5,
};

enum FrameFormat : int32_t {
  kFrameFormatUnknown = 0,
  kFrameFormatI420 = 1,
};

static constexpr uint32_t kSharedMemoryRingMagic = 0x334d4341u;  // "ACM3" LE.
static constexpr uint16_t kSharedMemoryRingVersion = 1;
static constexpr uint32_t kSharedMemoryRingSlotCount = 8;
static constexpr uint32_t kSharedMemoryNoSlot = 0xffffffffu;

struct SharedMemoryRingSlot {
  uint64_t begin_generation;
  uint64_t end_generation;
  int64_t pts_us;
  uint32_t data_offset;
  uint32_t data_size;
  uint32_t reserved;
};

struct SharedMemoryRingHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t slot_count;
  int32_t width;
  int32_t height;
  int32_t format;
  uint32_t slot_size;
  uint64_t latest_generation;
  uint32_t latest_slot;
  uint32_t reserved;
  SharedMemoryRingSlot slots[kSharedMemoryRingSlotCount];
};

inline size_t I420FrameSize(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) return 0;
  return static_cast<size_t>(width) * static_cast<size_t>(height) +
         2u * static_cast<size_t>((width + 1) / 2) *
             static_cast<size_t>((height + 1) / 2);
}

inline size_t SharedMemoryRingSize(int32_t width, int32_t height) {
  const size_t slot_size = I420FrameSize(width, height);
  if (slot_size == 0) return 0;
  return sizeof(SharedMemoryRingHeader) +
         static_cast<size_t>(kSharedMemoryRingSlotCount) * slot_size;
}

}  // namespace awesomecam
