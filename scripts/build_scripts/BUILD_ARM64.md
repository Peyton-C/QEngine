# arm64 Engine OS
QEngine runs RK3588 Engine firmware in QEMU, including RANE SYSTEM ONE and Prime 4 G2. Install Docker, QEMU, Python 3, e2fsprogs and `file`; [Build](../../docs/BUILDING.md) covers host setup and [macOS virgl](../../docs/MACOS_VIRGL.md) covers GPU rendering on macOS.

## Build
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --product-code RMZ2 --firmware /path/to/SYSTEMONE-5.0.4-Update.img
scripts/qemu/run_instance.sh --name rmz2-5.0.4
```

Point `--firmware` at the device's update image or desktop updater. QEngine builds the rootfs, data/factory disk and shared arm64 kernel. The default product code is `RMZ2`; choose another supported identity with `--product-code`. The [device table](../../README.md#emulated-controllers) lists booted firmware versions.

| Option | Effect |
| --- | --- |
| `--product-code CODE` | Selects the product identity |
| `--montero` / `--no-montero` | Enables or removes MONTERO and the mods in `shims/montero/mods` |
| `--force` | Rebuilds the rootfs, preserving the data/factory disk |
| `--size BYTES` | Changes rootfs capacity from 4 GiB |

Product-code and MONTERO choices survive `--force`. Creation rewrites `instance.env`, so save and reapply custom launch settings. See [Instances](INSTANCES.md) for ports, configuration and rebuilds.

## Display and controls
Engine uses EGLFS/KMS and Mesa. QEMU supplies the window or VNC server; the firmware's Qt 6 has no VNC platform plugin. Select `--display egl-vnc` for VNC with virgl, or `--no-gl` for guest software rendering.

For JP22, add `GPU_MAX_OUTPUTS=3` to `instance.env` before booting. Engine requires three separate screens; the builder supplies their matching display and touch configuration. Leave this setting out for single-screen products.

`touchbridge` converts QEMU tablet input into touch. `midisurface` supplies the selected product's MIDI panel and transport controls. On RMZ2, it also turns motorized mode off so playback does not wait for physical platter timecode. See [Audio and MIDI](../../docs/engine/AUDIO_MIDI.md) for commands and controller mappings.

## Runtime
| Component | QEngine handling |
| --- | --- |
| Identity | `dtshim` supplies product, serial, rotation and hardware stand-ins |
| Graphics | Matching virgl Mesa and `drmatomic` adapt Qt to virtio-gpu |
| Audio | `alsashim` adapts card discovery, formats and buffering |
| TEE | `teeshim` answers the check in Engine 5.1.0 and later |
| Shutdown | `quitshim` gives Engine its clean exit on service stop |

These shims leave the application binary unmodified. [Modern Engine OS](../../docs/engine/MODERN.md) covers the platform and its limits. The [support matrix](../../README.md#engine-os-support-matrix) separates boot, input, playback and other features; playback was heard on RMZ2 5.0.4; other identities and releases need their own checks.

To run an arm64 instance without QEMU on a Pi, follow the [Pi guide](../pi/README.md). SoundSwitch does not work in that chroot.
