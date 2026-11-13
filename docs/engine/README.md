# Engine OS
Engine OS is inMusic's Linux distribution for standalone DJ players and controllers. Engine is the DJ application it runs. QEngine runs that application unmodified, with a replacement kernel, graphics drivers and preloaded shims around it.

## Eras
QEngine selects a runtime from the firmware's libraries and filesystem, rather than its version number alone. Modern spans both architectures; RK3288 does not always mean classic.

| Era | Engine firmware | Platform | QEngine runtime |
| --- | --- | --- | --- |
| [Modern](MODERN.md) | arm64 4.4.0 onward; armv7 5.0.0 onward | Yocto, Qt 6, Mesa | EGLFS/KMS with virgl |
| [Classic](CLASSIC.md) | armv7 2.4.0 to 4.3.4 | Buildroot, Qt 5.15.2, proprietary Mali libraries | Mesa replaces Mali; EGLFS/KMS with virgl |
| [Legacy](LEGACY.md) | armv7 1.x to 2.3.x, glibc older than 2.34 | Older Buildroot; Qt 5.5, 5.9, 5.12 or 5.15 | Guest-linked shims; software rendering over Engine VNC |

These ranges describe the eras, not every release tested. The [device tables](../../README.md#emulated-controllers) list booted versions and the [support matrix](../../README.md#engine-os-support-matrix) separates display, input, audio and other features.

## Topics
| Topic | Contents |
| --- | --- |
| [Identity](IDENTITY.md) | Product codes, devicetree properties, IRQs and hardware checks |
| [Storage](STORAGE.md) | Application paths, settings, partitions and services |
| [Display and input](DISPLAY.md) | Mesa, EGLFS, VNC, rotation and touch bridges |
| [Audio and MIDI](AUDIO_MIDI.md) | ALSA, virtual control surfaces, controller mappings and motorized decks |
| [Telemetry](../BLOCKING_TELEMETRY.md) | Crash reporting and analytics blocking |
| [Remote access](../REMOTE_ACCESS.md) | Network access to a guest |

For build commands, use the [arm64 guide](../../scripts/build_scripts/BUILD_ARM64.md), [armv7 guide](../../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) or [Pi guide](../../scripts/pi/README.md). [BUILDING.md](../BUILDING.md) covers extraction, toolchains, disks and kernels.
