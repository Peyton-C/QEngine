# Instances
QEngine keeps each emulated device in `build/instances/<name>/`, with its own rootfs, writable disks, ports and configuration. Kernels and initrds are shared per architecture. Use separate instances to run different devices or firmware versions side by side.

## Create
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --product-code RMZ2 --firmware /path/to/SYSTEMONE-5.0.4-Update.img
```

`--firmware` accepts an update image or desktop updater. QEngine detects the family and architecture, builds the rootfs, creates missing writable disks and prepares the matching kernel. See [Build](../../docs/BUILDING.md) for requirements and extraction details.

| Option | Effect |
| --- | --- |
| `--name NAME` | Selects the instance directory |
| `--firmware PATH` | Supplies firmware, required on every invocation |
| `--device FAMILY` | Selects `engine`, `mpc`, `mpc-sample`, `drummodule` or `headrush`; QEngine checks the rootfs matches |
| `--product-code CODE` | Selects the identity built into the rootfs |
| `--montero` / `--no-montero` | Enables or removes MONTERO for Qt 6 Engine |
| `--size BYTES` | Sets rootfs capacity, normally 4 GiB |
| `--force` | Rebuilds the rootfs while keeping writable disks |
| `--binwalk` | Uses unverified extraction for this invocation |

Use a new name for another device or firmware version. Build one architecture at a time.

## Run
```sh
scripts/qemu/run_instance.sh --list
scripts/qemu/run_instance.sh --name rmz2-5.0.4
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display egl-vnc
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --no-gl
```

The launcher reads `instance.env` and starts QEMU. It prints the serial-console setup, SSH forwarding and applicable VNC addresses. Log in as `root` without a password on the serial console. The forwarded SSH port reaches guest port 22 when the guest runs an SSH server.

Different names can produce the same port allocation. If a port is occupied, change `SSH_PORT`, `VNC_DISPLAY` or `ENGINE_VNC_PORT` in that instance's configuration. QEMU VNC listens on TCP port `5900 + VNC_DISPLAY`.

Legacy Engine serves its interface at the separate `Engine VNC` address. QEMU VNC shows its console. See the [legacy guide](../../docs/engine/LEGACY.md) for viewer requirements and input limits.

## Display
| Mode | Output | Rendering |
| --- | --- | --- |
| `sdl-gl` | Local window | Host GPU through virgl |
| `egl-vnc` | VNC | Host GPU through virgl |
| `sdl` | Local window | Guest software renderer |
| `cocoa` | macOS window | Guest software renderer |
| `vnc` | VNC | Guest software renderer |
| `none` | No graphical output | No display backend |

`--display MODE` overrides the recorded mode for one run. `--gl` promotes SDL to `sdl-gl`, or VNC and Cocoa to `egl-vnc`. `--no-gl` selects the software mode; from `egl-vnc`, it selects `vnc`. These flags compose with `--display`.

New instances record `sdl-gl` on Linux. On macOS, they record `egl-vnc` when QEngine's local virgl QEMU build exists, otherwise `cocoa`. Legacy instances record `vnc` for their console and keep their own software-rendered application display.

If QEMU lacks the requested GL backend, the launcher warns and falls back to software rendering. See [macOS virgl](../../docs/MACOS_VIRGL.md) for the macOS build, and [Display and input](../../docs/engine/DISPLAY.md) for Engine's guest graphics stack.

## Configuration
Edit `build/instances/<name>/instance.env` to keep launch settings between runs. The file uses shell assignments; quote paths containing spaces.

| Key | Meaning |
| --- | --- |
| `INSTANCE_NAME` | Recorded instance name; launch selection uses `--name` |
| `DEVICE` | Firmware family |
| `FIRMWARE_IMG` | Recorded source firmware path |
| `ARCH` | Detected architecture, `arm64` or `armhf` |
| `PRODUCT_CODE` | Requested build identity; blank uses the builder's default |
| `MONTERO` | Recorded MONTERO build choice |
| `ROOTFS_IMG` | Application rootfs image |
| `DATA_IMG` | Writable data disk |
| `SD_IMG` | Extra internal SD image, empty for families without one |
| `ROOT_UUID` | Recorded filesystem UUID; the launcher rereads it from the image |
| `KERNEL_IMG`, `INITRD_IMG` | Shared kernel and initrd paths |
| `DISPLAY_MODE` | Persistent display choice |
| `SSH_PORT` | Host TCP port forwarded to guest port 22 |
| `VNC_DISPLAY` | QEMU VNC display number |
| `ENGINE_VNC_PORT` | Legacy Engine's host port forwarded to its VNC touch bridge |
| `SMP` | CPU count; legacy instances record 4, otherwise the launcher defaults to 8 |
| `GPU_MAX_OUTPUTS` | Optional display count; set 3 for JP22 |

Changing `PRODUCT_CODE` or `MONTERO` in this file does not modify an existing rootfs. Rebuild with the corresponding options. `DEVICE` and `ARCH` must agree with the built firmware; use another instance for a different target.

Pass other QEMU overrides through the environment:

```sh
MEM=2048 QEMU_AUDIO=off scripts/qemu/run_instance.sh --name rmz2-5.0.4
```

| Override | Effect |
| --- | --- |
| `QEMU_BIN` | Selects the QEMU executable |
| `ACCEL`, `CPU` | Overrides automatic accelerator and CPU selection |
| `MEM` | RAM in MiB, default 4096 on arm64 or 2048 on armhf; armhf accepts at most 3072 |
| `QEMU_AUDIO=off` | Omits the emulated output card; guest dummy or loopback cards remain |
| `AUDIODEV_BACKEND` | Overrides the host audio backend and its options |
| `RENDERNODE` | Selects the Linux render node for `egl-vnc` |
| `SCREEN` | Guest display size as `WIDTHxHEIGHT`; the default follows the family |
| `SERIAL` | Serial/monitor backend, default `mon:stdio` |
| `KERNEL_EXTRA_ARGS` | Additional kernel command-line arguments |
| `QEMU_EXTRA_ARGS` | Additional QEMU arguments, split on whitespace |

For a persistent override from this second table, use an exported assignment such as `export MEM=2048` in `instance.env`. Plain assignments for these keys are not forwarded by `run_instance.sh`. Recorded keys such as `SMP` take precedence over an incoming environment value.

## Rebuild
Stop the guest, then rerun creation with `--force`:

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --firmware /path/to/SYSTEMONE-5.0.4-Update.img --force
```

QEngine keeps the recorded product-code and MONTERO choices unless you override them. It keeps `data_disk.img`, `emmc.img` and `sdcard.img`, so guest settings and writable overlays survive. Pass `--size` again if you need a non-default rootfs size.

Every creation run rewrites `instance.env`, even when it reuses a completed rootfs. Save custom ports, display settings and other overrides before running it, then reapply them. Without `--force`, changed firmware or build options do not replace an existing completed rootfs. Failed rootfs builds lack `.rootfs.complete` and are rebuilt on the next attempt.

## Files
| Path | Contents |
| --- | --- |
| `build/instances/<name>/rootfs.img` | Built rootfs |
| `build/instances/<name>/data_disk.img` | Data/factory disk on newer platforms |
| `build/instances/<name>/emmc.img` | Single-partition data disk on RK3288 platforms |
| `build/instances/<name>/sdcard.img` | Alesis drum module's internal SD |
| `build/instances/<name>/instance.env` | Instance configuration |
| `build/instances/<name>/.rootfs.complete` | Successful rootfs-build marker |
| `build/vmlinuz-generic-<arch>`, `build/initrd-generic-<arch>` | Kernel and initrd shared by architecture |

Back up writable disks before resetting guest state. With the guest stopped, removing its data or SD image lets the next creation run make a blank replacement. Copy sparse images with `rsync --sparse`; check allocated storage with `du -h`.

## Stopping
Run `poweroff` in the guest and wait for QEMU to exit before rebuilding or copying images. Never run two guests against the same disks. The launcher takes an instance lock when `flock` is available; otherwise QEMU's image locking is the remaining guard.

For feature support, use the [device tables](../../README.md). Target-specific instructions live in [arm64 Engine](BUILD_ARM64.md), [armv7 Engine](BUILD_ARMV7_ENGINE.md), [MPC](BUILD_MPC.md), [MPC Sample](BUILD_MPC_SAMPLE.md), [Alesis](BUILD_ALESIS.md) and [HeadRush](BUILD_HEADRUSH.md).
