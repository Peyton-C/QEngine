# HeadRush
QEngine boots HeadRush Prime and VX5 with Docker, QEMU, Python 3 and e2fsprogs. Point `--firmware` at the update image or desktop updater; QEngine selects the architecture and builder from its contents.

| Device | Code | Firmware tried | Architecture | Display | Limits |
| --- | --- | --- | --- | --- | --- |
| Prime | `HG06` | 5.2.0 | armv7, RK3288 | Landscape with touch | Silent loopback audio |
| VX5 | `HV03` | 1.2.2 | arm64, RK3566 | 160×128, landscape | Silent loopback audio, no control surface |

## Build
```sh
scripts/build_scripts/new_instance.sh --name headrush-vx5-1.2.2 --product-code HV03 --firmware HeadRush-VX5-1.2.2.img
scripts/qemu/run_instance.sh --name headrush-vx5-1.2.2 --display vnc
```

Use a different instance name for each device and firmware version. `--force` rebuilds the rootfs and keeps the data disk. See [INSTANCES.md](INSTANCES.md) for display options and host ports.

VX5 uses the shared JUCE builder, `build_mpc_rootfs.sh`, and `HV03.service`. Prime uses the armv7 Engine builder and `evil.service`. VX5 needs no Qt or EGL replacement and no rotation shim. Both use QEngine's generic kernel, not the device kernel in the updater.

## Audio
QEngine supplies a kernel loopback card because both applications need an audio device to start. `pcmhold` keeps it at the device's channel counts and rate. It feeds capture silence and discards playback; this does not provide host audio.

| Device | Playback | Capture | Rate |
| --- | --- | --- | --- |
| Prime | 8 channels | 2 channels | 96000 Hz |
| VX5 | 2 channels | 2 channels | 48000 Hz |

VX5 finds its card through `/dev/snd/by-path/platform-sound`, which a QEngine udev rule assigns to the loopback card. `qengine-vx5-audio.service` prepares it before `HV03.service` starts. The VX5 unit also disables glibc's small-block allocation cache and zero-fills fresh allocations to avoid a DSP startup crash in 1.2.2.

If an older generic kernel reports that `snd-aloop` is missing, shut down the guest and rebuild the shared kernel and initrd for its architecture:

```sh
scripts/build_scripts/get_kernel.sh --arch arm64
```

## Controls
VX5's physical encoders, switches and expression input are not emulated. Missing control-input, LED and phantom-power paths produce assertions in the application journal. A rendered preset screen does not establish that effects or preset navigation work. Newer VX5 firmware has not been tried.
