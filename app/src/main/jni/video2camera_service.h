#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

namespace awesomecam {

struct VideoTargetState {
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t generation = 0;
  uint64_t last_seen_ns = 0;
  uint64_t hit_count = 0;
};

struct SourceFrameStatus {
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t generation = 0;
  uint32_t slot = 0;
  int64_t pts_us = 0;
  uint64_t registration_generation = 0;
};

struct SharedMemoryFrameView {
  const uint8_t *bytes = nullptr;
  size_t size = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t generation = 0;
  uint32_t slot = 0;
  int64_t pts_us = 0;
  std::shared_ptr<const void> keepalive;
};

bool EnsureVideo2CameraServiceStarted();
bool ProbeBinderRuntimeOnly();
bool ProbeBinderClassDefineOnly();
bool ProbeBinderNewOnly();
bool ProbeBinderThreadPoolOnly();
bool ProbeBinderFullServiceWorkerAsync();
bool ProbeBinderThreadPoolWorkerOnly();
bool ProbeClassicServiceWorkerAsync();
bool ProbeClassicThreadPoolWorkerOnly();
void StartVideo2CameraServiceAsync();

void UpdateVideo2CameraTarget(int32_t width, int32_t height, int32_t format);
bool PeekVideo2CameraTarget(VideoTargetState *out);
bool GetVideo2CameraTargets(std::vector<VideoTargetState> *out);
bool CopyLatestSourceFrame(SharedMemoryFrameView *out);
bool GetSourceFrameStatus(SourceFrameStatus *out);
void ClearSharedMemoryRegistration();

}  // namespace awesomecam
