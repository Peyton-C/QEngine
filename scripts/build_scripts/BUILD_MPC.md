# MPC and Force
QEngine runs Gen 1 and Gen 2 MPC firmware in QEMU, with touch, a virtual panel and host audio. Install Docker, QEMU, Python 3, e2fsprogs and `file`; see [Build](../../docs/BUILDING.md) for host setup. [MPC Sample](BUILD_MPC_SAMPLE.md) and [Alesis](BUILD_ALESIS.md) have separate guides.

## Build
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name mpc-one-3.10.0 \
    --product-code ACVA3 --firmware /path/to/MPC-3.10.0-Gen2-update.img
scripts/qemu/run_instance.sh --name mpc-one-3.10.0
```

QEngine detects the image's generation and architecture. Use `--product-code` to select a model. `--force` rebuilds the rootfs and keeps its writable disk; [Instances](INSTANCES.md) covers configuration and launch options.

| Image | Platform | Default identity | Data disk |
| --- | --- | --- | --- |
| Gen 1 | RK3288, armv7 | `ACV5`, MPC X | `emmc.img`, single `az01-internal` partition |
| Gen 2 | RK3588, arm64 | `ACVA3`, MPC One (Gen 2) | `data_disk.img`, `/data` and `/factory` |

The [device table](../../README.md#mpc) lists booted versions and Gen 1 codes.

## MPC 2.x
MPC 2.x is Gen 1 on Buildroot, with glibc 2.28 to 2.32, and starts MPC from `inmusic-mpc.service` rather than `acvs.service`. QEngine links the shims against the image's own libraries and attaches them to that unit, so the same build command works. 2.7.0, 2.9.1 and 2.11.7 boot as `ACV5` with touch, the panel and output; other codes are untried on 2.x.

2.x opens the dummy card as S32, so QEngine loads it with the `qengine-ac50` model, as for MPC Sample. 2.9.1 powers the guest off if the card refuses S32. 2.7.0 predates the `platform-sound` lookup and takes the first ALSA device whose name contains "MPC", so QEngine adds a PCM named that over the dummy card and points `default` at it.

## Gen 2
| Model | Code |
| --- | --- |
| MPC One (Gen 2) | `ACVA3` |
| MPC Key 37 (Gen 2) | `ACVR2` |
| MPC Key 61 (Gen 2) | `ACVM2` |
| MPC XL | `ACVC` |
| MPC Live III | `ACVG` |
| Force (Gen 2) | `ADA2S` |

The Gen 2 image also runs as Gen 1 products: `ADA2`, `ACVA`, `ACV5` and `ACVB` have played pads on 3.10.0. That lets those identities use arm64 host acceleration. Running a Gen 1 image as a Gen 2 product is untested.

## Input
QEngine starts `touchbridge_mpc.service` before MPC's unit, since MPC discovers touchscreens only at startup. The bridge turns QEMU's tablet into a touchscreen. Hold scripted taps briefly; an immediate press and release may not register.

`mpcsurface` supplies the product's panel. Write commands in the guest:

```sh
echo 'pad 1 120' > /run/mpcsurface.fifo
```

Use the [surface reference](../../shims/mpcsurface/README.md) for buttons, pad layout, Q-Links and logging. All Gen 1 and Gen 2 panels were tried on 3.10.0. Key keybeds are not emulated; their default Fabric instrument content is absent. XL and Live III accept classic pad notes, without MPCe expression.

## Audio
MPC opens `/dev/snd/by-path/platform-sound`, which QEngine points at `snd-dummy`. `qengine-mpc-audio.service` loads that card before MPC starts. `alsashim` copies the first stereo pair to QEMU's output card. Inputs are silent.

| Card | Role |
| --- | --- |
| `snd-dummy` | MPC's internal audio device, with its original period size |
| QEMU HDA on arm64, virtio-sound on armv7 | Host output receiving the copied stereo pair |
| `snd-aloop` | Unsuitable as MPC's internal card; streams stall |

If MPC reports "No internal audio device could be found!", stop the guest and refresh its architecture's kernel and initrd with `get_kernel.sh --arch arm64` or `--arch armhf`. Older QEngine kernels may lack the required `snd-dummy` module.

Output has additional latency and can tick as the two cards' clocks drift. `QEMU_AUDIO=off` leaves MPC playing to its dummy card without host output. Audio was heard on arm64 `ACVA3`, 3.10.0 through CoreAudio; Gen 1 3.10.0 output was recorded through QEMU's WAV backend but not listened to.

## Identity and storage
`dtshim` serves the instance's product code and serial to the application. To inspect what the firmware library reports, run in the guest:

```sh
LD_PRELOAD=/root/dtshim.so az0x-info
```

A login shell does not inherit the application's preload. Gen 2 can still report "No data available" and log an unknown board platform without preventing the interface from starting.

QEngine does not serve the Gen 2 `chosen` flags `acvs-loopback-content`, `emmc-synths` or `nvme-fitted`. Their storage/content/NVMe mounts are skipped; user-content persistence is unverified. Force Gen 2 can warn that storage is almost full. Firmware updates do not include every factory instrument library.
