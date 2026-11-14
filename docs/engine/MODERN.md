# Modern Engine OS
Modern Engine OS uses Yocto, Qt 6 and Mesa. It ships on RK3588 arm64 devices from Engine 4.4.0 and on RK3288 armv7 devices from 5.0.0. The examined 4.6.0 and 5.0.4 roots use Qt 6.7.2 and glibc 2.39; a device's architecture does not determine its Qt generation.

## Runtime
QEngine runs Engine through EGLFS/KMS on virtio-gpu. These builds have no Qt VNC platform plugin, so connect to QEMU's display or VNC server. `touchbridge` turns QEMU tablet events into touchscreen input. See [Display and input](DISPLAY.md).

| Component | QEngine change |
| --- | --- |
| Mesa | Adds virgl to the rootfs's DRI or libgallium layout |
| Display configuration | Selects the virtual DRM device, connector and screen size |
| `drmatomic` | Converts Qt's framebuffer format and modesetting calls for virtio-gpu |
| `dtshim` | Supplies product identity, panel rotation and hardware stand-ins |
| `alsashim` | Adapts ALSA card enumeration, formats, buffers and MIDI clients |
| `midisurface` | Answers the product's MIDI identity inquiry and supplies transport controls |
| `teeshim` | Answers the TEE check on RK3588 Engine 5.1.0 and later |
| `quitshim` | Turns service shutdown into Engine's own clean exit |

QEngine keeps the vendor Qt runtime. MONTERO can load QML mods on Qt 6; enable it with `new_instance.sh --montero`.

## Architecture
| Target | Firmware layout | QEngine handling |
| --- | --- | --- |
| RK3588, arm64 | Read-only root with separate `/data` and `/factory`; Mesa for the physical Mali-G610 | Builds with `build_arm64_rootfs.sh`; supplies a data/factory disk and virgl |
| RK3288, armv7 | Single `az01-internal` data partition; newer Qt and Mesa on the older SoC | Builds with `build_armv7_engine_rootfs.sh`; supplies the Gen 1 data-disk layout |

The RK3588 firmware uses open-source Panthor/Panfrost drivers rather than the proprietary `libmali` stack. QEMU uses virgl instead. On armv7, time64 symbol names matter to preloaded shims: `drmatomic` exports both `ioctl` and `__ioctl_time64`.

## Limits
RMZ2 starts with motorized platters enabled, so QEngine disables that mode through `midisurface` before normal playback. QEngine supplies a full battery on AC for Prime GO, Prime GO+ and Mixstream Pro GO; their native identities boot on 5.3.0. See [Identity](IDENTITY.md) and [Audio and MIDI](AUDIO_MIDI.md).

The repository records audible playback on arm64 RMZ2 5.0.4. That does not establish playback on modern armv7 or every product identity. Consult the [support matrix](../../README.md#engine-os-support-matrix) before choosing an instance.

For commands, use the [arm64 guide](../../scripts/build_scripts/BUILD_ARM64.md) or [armv7 guide](../../scripts/build_scripts/BUILD_ARMV7_ENGINE.md). The [Pi guide](../../scripts/pi/README.md) covers running an arm64 rootfs in a chroot, including Wayland and its SoundSwitch limit.
