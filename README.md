# QEngine
QEngine runs inMusic's music-device applications unmodified in QEMU, including Engine, MPC and HeadRush. It also runs arm64 Engine OS in a chroot on the [Raspberry Pi](scripts/pi/README.md).

## Setup
Install Docker, QEMU including `qemu-img`, Python 3, e2fsprogs and `file`, then start Docker. Run from the repository root:

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --firmware /path/to/SYSTEMONE-5.0.4-Update.img
scripts/qemu/run_instance.sh --name rmz2-5.0.4
```

Point `--firmware` at an update image or desktop updater. QEngine detects the family and architecture, builds the rootfs and prepares the disks and kernel. Use `--product-code CODE` to choose a device identity, and a different instance name for each device or firmware version. Build one architecture at a time.

The launcher prints connection details; the serial-console root login has no password. Legacy Engine uses the separate `Engine VNC` address for its interface. Run `poweroff` in the guest when finished.

See [Build](docs/BUILDING.md) for requirements, rebuilds and display options. On macOS, use the [virgl guide](docs/MACOS_VIRGL.md) for GPU rendering.

## Engine OS support matrix
Y means supported in the builds tried, N means unavailable, and ? means unverified or unresolved. Results do not cover every device and firmware combination. The [era guides](docs/engine/README.md) describe the runtime limits.

|                         | Modern arm64 | Modern armv7    | Classic         | Legacy |
|-------------------------|--------------|-----------------|-----------------|--------|
| Engine                  | Y            | Y               | Y               | Y      |
| SoundSwitch             | Y            | ?               | ?               | ?      |
| Display                 | Y            | Y               | Y               | Y      |
| Virgl                   | Y            | Y               | Y               | N      |
| Touch input             | Y            | Y               | Y               | Y      |
| Keyboard navigation     | Y            | Y               | Y               | ?      |
| Audio playback          | Y            | Y (chroot only) | Y (chroot only) | ?      |
| MIDI                    | Y            | Y               | Y               | ?      |
| External media (USB/SD) | Y            | Y               | Y               | ?      |

Legacy input checks cover single taps, playback and MIDI remain unverified. SoundSwitch does not work in the [Pi chroot](scripts/pi/README.md#known-limits).

## OS Eras
QEngine primarly targets Modern and Classic inMusic firmware versions, but there is basic support for some Legacy era firmware versions.

| Platform                   | Modern  | Classic     | Legacy  |
|----------------------------|---------|-------------|---------|
| Engine                     | 4.4.0+  | 2.4.0-4.3.4 | < 2.4.0 |
| MPC                        | 3.4.1+  | –           | 2.x     |
| MPC Force                  | 3.5.0+  | –           | < 3.5.0 |
| MPC Sample                 | 1.x     | –           | –       |
| Alesis drum module         | 1.5.2.3 | –           | –       |
| HeadRush VX5               | 1.x     | –           | –       |
| HeadRush Prime, Flex Prime | –       | 5.2.0       | –       |
| Sheeran Looper X           | –       | 1.0.2       | –       |
| HeadRush Pedalboard        | –       | –           | 2.x     |
| HeadRush Looperboard       | –       | –           | 2.x     |

## Emulated controllers
Code selects the product identity with `--product-code`. Version cells list firmware booted; ranges summarize checked releases. ? means unverified or unresolved, and – means no release in that major. ! marks Prime GO firmware booted as `JC11` rather than its native `JP11` identity.

|                         | Code  | SoC    | Signed | Arch  | 1.x         | 2.x   | 3.x   | 4.x           | 5.x         |
|-------------------------|-------|--------|-----------|-------|-------------|-------|-------|---------------|-------------|
| Denon DJ Prime 2        | JC16  | RK3288 | N         | armv7 | 1.5.2, 1.6.2 | ?     | ?     | ?             | 5.3.0       |
| Denon DJ Prime 4        | JC11  | RK3288 | N         | armv7 | 1.3.2-1.4.1, 1.5.2-1.6.2 | 2.4.0 | 3.4.0 | 4.0.0-4.3.0   | 5.3.0       |
| Denon DJ Prime 4+       | JC11S | RK3288 | Y         | armv7 | –           | –     | ?     | ?             | 5.3.0       |
| Denon DJ Prime 4 G2     | JP24  | RK3588 | Y         | arm64 | –           | –     | –     | –             | 5.1.0-5.3.0 |
| Denon DJ Prime GO       | JP11  | RK3288 | N         | armv7 | 1.6.2 !     | ?     | ?     | 4.3.0 !       | 5.3.0 !     |
| Denon DJ Prime GO+      | JP11S | RK3288 | Y         | armv7 | –           | –     | –     | ?             | ?           |
| Denon DJ SC5000 Prime   | JP07  | RK3288 | N         | armv7 | 1.0.1-1.3.1, 1.4.1-1.6.2 | ?     | ?     | ?             | 5.0.4-5.3.0 |
| Denon DJ SC5000M Prime  | JP08  | RK3288 | N         | armv7 | 1.3.1-1.6.0 | ?     | ?     | ?             | 5.3.0       |
| Denon DJ SC6000 Prime   | JP13  | RK3288 | N         | armv7 | 1.5.2, 1.5.3, 1.6.2 | ?     | ?     | ?             | 5.3.0       |
| Denon DJ SC6000M Prime  | JP14  | RK3288 | N         | armv7 | 1.5.2, 1.6.0 | ?     | ?     | ?             | 5.3.0       |
| Denon DJ SC Live 2      | JP20  | RK3288 | Y         | armv7 | –           | 2.3.3 | ?     | ?             | 5.3.0       |
| Denon DJ SC Live 4      | JP21  | RK3288 | Y         | armv7 | –           | ?     | ?     | 4.3.4         | 5.3.0       |
| Numark Mixstream Pro    | NH08  | RK3288 | N         | armv7 | –           | 2.0.1-2.3.2 | ?     | ?             | 5.3.0       |
| Numark Mixstream Pro+   | NH08S | RK3288 | Y         | armv7 | –           | ?     | ?     | ?             | 5.3.0       |
| Numark Mixstream Pro GO | NH10  | RK3288 | Y         | armv7 | –           | –     | ?     | ?             | ?           |
| RANE SYSTEM ONE         | RMZ2  | RK3588 | Y         | arm64 | –           | –     | –     | 4.5.0-4.6.0   | 5.0.4-5.3.0 |

Prime GO's native identity can quit on its battery check. Prime GO+ and Mixstream Pro GO remain unresolved under their native identities. Early Prime 4 builds have input limits. See [Identity](docs/engine/IDENTITY.md#battery-identities) and the [armv7 guide](scripts/build_scripts/BUILD_ARMV7_ENGINE.md#legacy-firmware) before choosing those targets.

## MPC
Gen 1 and Gen 2 firmware use the same build command, with architecture detected from the image. The table lists booted versions, not full hardware support; MPC 2.x remains untested.

|                    | Code  | SoC    | Signed | Arch  | 2.x | 3.x          |
|--------------------|-------|--------|-----------|-------|-----|--------------|
| MPC X              | ACV5  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC X SE           | ACV5S | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC XL             | ACVC  | RK3588 | Y         | arm64 | –   | 3.10.0       |
| MPC Live           | ACV8  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC Live Mk 2      | ACVB  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC Live III       | ACVG  | RK3588 | Y         | arm64 | –   | 3.10.0       |
| MPC One            | ACVA  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC One+           | ACVA2 | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC One (Gen 2)    | ACVA3 | RK3588 | Y         | arm64 | –   | 3.10.0       |
| MPC Key 61         | ACVM  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC Key 61 (Gen 2) | ACVM2 | RK3588 | Y         | arm64 | –   | 3.10.0       |
| MPC Key 37         | ACVR  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1-3.10.0 |
| MPC Key 37 (Gen 2) | ACVR2 | RK3588 | Y         | arm64 | –   | 3.10.0       |
| Force              | ADA2  | RK3288 | ?         | armv7 | –   | 3.10.0       |
| Force (Gen 2)      | ADA2S | RK3588 | Y         | arm64 | –   | 3.10.0       |

QEngine supplies a virtual panel and host audio with silent inputs. Audio was heard on arm64 `ACVA3`, 3.10.0; Gen 1 output was recorded but not listened to. Keybeds and some factory instrument content are absent. See the [MPC guide](scripts/build_scripts/BUILD_MPC.md) for controls, audio and storage limits.

## Other devices
? means untested; a listed code alone does not establish support.

| Device | Code | Architecture | Firmware booted | Limits |
| --- | --- | --- | --- | --- |
| Akai MPC Sample | `AC50` | arm64, RK3566 | 1.3.0 | Host audio and virtual panel; factory library absent |
| Alesis Prime Drum Module | `LDMD` | armv7, RK3288 | 1.5.2.3 | Kit screen and touch; no audio device or full sample library |
| Alesis Core Drum Module | `LDMF` | armv7, RK3288 | ? | Build refused, content partition missing |
| HeadRush Prime | `HG06` | armv7, RK3288 | 5.2.0 | Touch; silent loopback audio |
| HeadRush VX5 | `HV03` | arm64, RK3566 | 1.2.2, 1.3.1 | Host audio and virtual panel; input from a WAV file |
| Sheeran Looper X | `HG08` | armv7, RK3288 | 1.0.2 | Four-track UI; factory content absent; silent loopback audio |
| HeadRush Core | `HV01` | ? | ? | Untested |
| HeadRush Gigboard | `HG02` | ? | ? | Untested |
| HeadRush Looperboard | `HG03` | ? | ? | Untested |
| HeadRush MX5 | `HG04` | ? | ? | Untested |
| HeadRush Flex Prime | `HG12` | ? | ? | Untested |
| HeadRush Pedalboard | `MG01` | ? | ? | Untested |

Use the [MPC Sample](scripts/build_scripts/BUILD_MPC_SAMPLE.md), [Alesis](scripts/build_scripts/BUILD_ALESIS.md) and [HeadRush](scripts/build_scripts/BUILD_HEADRUSH.md) guides for target-specific setup and limits. The VX5 audio and control checks cover 1.2.2; 1.3.1 is listed as booted.

## Documentation
| Guide | Contents |
| --- | --- |
| [Build](docs/BUILDING.md) | Requirements, firmware extraction, toolchains, disks and kernels |
| [Instances](scripts/build_scripts/INSTANCES.md) | Instance settings, ports and launch options |
| [Engine OS](docs/engine/README.md) | Modern, classic and legacy runtimes; shared internals |
| [arm64 Engine](scripts/build_scripts/BUILD_ARM64.md) / [armv7 Engine](scripts/build_scripts/BUILD_ARMV7_ENGINE.md) | Target build instructions |
| [MPC](scripts/build_scripts/BUILD_MPC.md) / [MPC Sample](scripts/build_scripts/BUILD_MPC_SAMPLE.md) / [Alesis](scripts/build_scripts/BUILD_ALESIS.md) / [HeadRush](scripts/build_scripts/BUILD_HEADRUSH.md) | Other target instructions |
| [Raspberry Pi](scripts/pi/README.md) / [FLX10](scripts/pi/flx10/README.md) | Chroot setup and controller integration |
| [macOS virgl](docs/MACOS_VIRGL.md) | QEMU with GPU rendering on macOS |
| [Remote access](docs/REMOTE_ACCESS.md) | Working with a remote guest |
| [Telemetry](docs/BLOCKING_TELEMETRY.md) | Telemetry blocking, applied to every built rootfs |

## Screenshots
| Denon DJ Prime 4, 4.3.0 | RANE SYSTEM ONE, 4.6.0 | Denon DJ SC5000 Prime, 1.0.1 |
| --- | --- | --- |
| ![Prime 4 library](images/DENON_DJ_PRIME_4_PLAYLISTS_430.png) | ![SYSTEM ONE library](images/RANE_SYSTEM_ONE_PLAYLISTS_460.png) | ![SC5000 player](images/DENON_DJ_SC5000_PLAY_101.png) |
