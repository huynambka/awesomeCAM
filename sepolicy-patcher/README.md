# sepolicy patcher

Minimal KernelSU/Magisk-style SELinux module for awesomeCAM native injector.

## What this allows
- `su` ptrace attach to and restart `cameraserver` for native injector upgrades.
- `cameraserver` load `/data/camera/*.so` labeled `system_lib_file`.
- `cameraserver` register `Video2CameraService`.
- awesomeCAM app find `Video2CameraService`.
- native-player decode path: `cameraserver` uses `media.extractor` and passes fd for `/data/camera/input.mp4`.
- app/cameraserver/media.extractor read `/data/camera/input.mp4` labeled `awesomecam_source_file`.

## Removed
Old Frida-style broad rules removed:
- no `frida_file` / `frida_memfd`
- no `allow domain domain process execmem`
- no zygote ptrace rules
- no broad `allow domain default_android_service service_manager find`

## File labels
`post-fs-data.sh` sets:
- `/data/camera` -> `system_data_file`
- `/data/camera/input.mp4` -> `awesomecam_source_file`
- `/data/camera/*.so` -> `system_lib_file`

## Install
Install `sepolicy_patcher.zip` in KernelSU Manager, then reboot.
Manual action relabels files only; policy loads at boot.
