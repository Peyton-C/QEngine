# Build
QEngine extracts an inMusic update, builds the shims and graphics libraries its application needs, and creates an instance ready to boot. Each instance owns its rootfs, writable disks and configuration. QEngine shares a kernel and initrd between instances of the same architecture.

## Requirements
Run commands from the repository root. Start Docker before building; the builders mount and modify the rootfs inside privileged Linux containers.

| Tool | Use |
| --- | --- |
| Docker with support for the target architecture | Compiles shims, builds libraries and prepares filesystems |
| Python 3 | Reads firmware containers and desktop updaters |
| QEMU, including `qemu-img` | Resizes images and runs the guest |
| e2fsprogs, including `debugfs` and `dumpe2fs` | Inspects the rootfs and reads its UUID |
| `file` | Checks extracted and built images |

`qemu-system-aarch64` can run both arm64 and armv7 guests. QEngine falls back to `qemu-system-arm` for armv7 when needed; `QEMU_BIN` selects a particular binary. On macOS, add Homebrew's keg-only e2fsprogs tools to `PATH`:

```sh
export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"
```

Build one architecture at a time. The builders reuse Docker image tags across architectures, so concurrent arm64 and armv7 builds can select the wrong cached image.

## Instance
Point `--firmware` at an update image, a zipped image, a Mac updater zip or a Windows updater executable. QEngine detects the family and architecture from the extracted rootfs.

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --firmware /path/to/SYSTEMONE-5.0.4-Update.img
scripts/qemu/run_instance.sh --name rmz2-5.0.4
```

The launcher prints the serial, SSH and VNC connection details. Log in as `root` without a password on the serial console. Legacy Engine instances have a separate `Engine VNC` address for the application; their QEMU VNC address shows the console.

| Build option | Effect |
| --- | --- |
| `--name NAME` | Creates or reuses `build/instances/NAME/` |
| `--firmware PATH` | Supplies the firmware, required on every invocation |
| `--device FAMILY` | Selects a family explicitly; QEngine checks it against the rootfs |
| `--product-code CODE` | Selects the product identity and records it for later rebuilds |
| `--montero` / `--no-montero` | Enables or removes MONTERO on Qt 6 Engine; records the choice |
| `--size BYTES` | Sets the rootfs image size, normally 4294967296 bytes, or 4 GiB |
| `--force` | Rebuilds the rootfs and keeps the writable disks |
| `--binwalk` | Uses unverified binwalk extraction for this invocation |

Use a new instance name for another device or firmware version. Use `--force` to apply changed shims, firmware or build options to an existing instance. Without it, QEngine keeps a completed rootfs. Stop that instance before rebuilding it.

`new_instance.sh` rewrites `instance.env` when it runs. It carries forward recorded product-code and MONTERO choices, but custom launch settings should be saved and reapplied after a rebuild. Pass `--size` again when rebuilding a non-default image size.

## Targets
| Family | Architecture | Guide |
| --- | --- | --- |
| `engine` | arm64, RK3588 | [arm64 Engine](../scripts/build_scripts/BUILD_ARM64.md) |
| `engine` | armv7, RK3288 | [armv7 Engine](../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) |
| `mpc` | armv7 or arm64 | [MPC and Force](../scripts/build_scripts/BUILD_MPC.md) |
| `mpc-sample` | arm64, RK3566 | [MPC Sample](../scripts/build_scripts/BUILD_MPC_SAMPLE.md) |
| `drummodule` | armv7, RK3288 | [Alesis drum modules](../scripts/build_scripts/BUILD_ALESIS.md) |
| `headrush` | armv7 or arm64 | [HeadRush](../scripts/build_scripts/BUILD_HEADRUSH.md) |

The [README](../README.md) lists booted firmware versions and feature support. For Engine OS runtime differences, read [modern](engine/MODERN.md), [classic](engine/CLASSIC.md) or [legacy](engine/LEGACY.md). The [Pi guide](../scripts/pi/README.md) covers staging an arm64 Engine instance for a chroot.

## Extraction
QEngine reads the firmware container directly and checks its embedded rootfs digest before writing the image. Digest checks detect damaged content; they do not authenticate the firmware's signature.

| Container | Reader |
| --- | --- |
| Plain FIT | `extract_fit_rootfs.py` |
| `AZ01` | `extract_az01_rootfs.py` |
| `AZ0x` | `extract_az0x_rootfs.py` |
| Desktop updater | `extract_updater.py`, followed by the matching container reader |

The readers live in [scripts/build_scripts/](../scripts/build_scripts/). Use `--binwalk` only for a container QEngine cannot read or to compare extraction results. That path does not verify the rootfs. Binwalk 3 cannot unpack plain FIT firmware, and binwalk 2 produces a broken rootfs from SYSTEM ONE 5.2.0. QEngine does not record this extraction override in `instance.env`.

## Rootfs
The family registry in `new_instance.sh` selects a builder from rootfs markers and the dynamic loader's architecture. `ld-linux-aarch64.so.1` selects arm64; `ld-linux-armhf.so.3` selects armhf. The firmware filename and product code do not determine the architecture.

The builder grows the raw image with `qemu-img`, checks the filesystem with `e2fsck`, and grows it with `resize2fs`. It compiles shims from source, installs the matching graphics libraries, and configures the application service and hardware stand-ins. Every builder runs the shared telemetry-blocking step. The firmware's application binary stays unmodified.

Shared steps live in [rootfs_steps/](../scripts/build_scripts/rootfs_steps/). Builders source them inside the privileged container; they are safe to apply again to the same image. See [Identity](engine/IDENTITY.md), [Display and input](engine/DISPLAY.md) and [Audio and MIDI](engine/AUDIO_MIDI.md) for Engine's shim mechanisms.

`new_instance.sh` writes `.rootfs.complete` only after the rootfs builder succeeds. If a build fails, rerun the same command; an image without that marker is rebuilt. Later failures, such as kernel preparation, can reuse the completed rootfs.

## Toolchain
Build userspace tools for the guest's architecture and against a glibc no newer than the guest's. The contemporary shim builders use Debian bookworm; legacy Engine shims link against the extracted firmware's own libraries. Qt-linked plugins also need matching Qt headers and libraries. Follow the target builder rather than copying libraries from the host.

Engine firmware older than glibc 2.34 takes the legacy software-rendering path, including some Qt 5.15.2 releases. That path keeps the native Qt runtime. The [armv7 guide](../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) covers `LEGACY_QT_HEADERS` when firmware omits its headers.

Kernel modules need the running kernel's exact build and symbol versions, independently of userspace glibc compatibility. The [experimental AZ04 audio guide](../shims/rk3588/az04-audio/README.md) covers those modules; they are not part of normal Engine playback.

## Disks
`make_disk.sh` creates the writable disks with the partition names and GUIDs the firmware's mount units expect. Normal data partitions are formatted by the guest on first boot. The drum module's internal SD image is prepared as ext4 by QEngine.

| Layout | Instance file | Contents |
| --- | --- | --- |
| `engine` | `data_disk.img` | `/data` and `/factory`, for RK3588 Engine, Gen 2 MPC, MPC Sample and VX5 |
| `mpc` | `emmc.img` | `az01-internal`, for RK3288 targets, including Engine |
| `internal-sd` | `sdcard.img` | Extra internal SD storage for the Alesis drum module |

Disk-layout names describe platform conventions. Armv7 Engine uses the `mpc` layout. On RK3288, the writable disk also backs `/etc` and `/var` overlays; on RK3588, `/etc` lives on the rootfs. See [Storage](engine/STORAGE.md).

Writable disks survive `--force`, including settings and any overlay files that hide rebuilt rootfs content. Back them up before resetting guest state. Keep each instance's disks together, and never run two guests against the same writable images.

Raw images are sparse. Use `du -h build/instances/*/rootfs.img` to see allocated storage; `ls -lh` reports virtual capacity. Preserve holes when copying with `rsync --sparse`.

## Kernel
`get_kernel.sh` prepares a generic Debian kernel and initrd for QEMU's `virt` machine. The initrd includes the storage, input, graphics and audio modules QEngine needs, and makes its modules available in the guest. A vendor Rockchip kernel targets hardware QEMU does not emulate and is not a substitute.

| Architecture | Shared files in `build/` | QEMU machine |
| --- | --- | --- |
| arm64 | `vmlinuz-generic-arm64`, `initrd-generic-arm64` | `virt,highmem=on` |
| armhf | `vmlinuz-generic-armhf`, `initrd-generic-armhf` | `virt,highmem=off` |

QEngine builds missing kernel files on demand. To refresh one architecture's pair, run `scripts/build_scripts/get_kernel.sh --arch arm64` or `--arch armhf`. Existing instances reference those shared paths.

Armv7 needs `highmem=off` for its PCI devices and accepts at most 3072 MiB of RAM. Its default accelerator is TCG unless an arm64 Linux host and QEMU support AArch32 guests under KVM. Apple Silicon cannot accelerate armv7 guests. Arm64 uses HVF or KVM where available, otherwise TCG.

## Display
```sh
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display egl-vnc
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --no-gl
```

| Mode | Output | Rendering |
| --- | --- | --- |
| `sdl-gl` | Local window | Host GPU through virgl |
| `egl-vnc` | VNC | Host GPU through virgl |
| `sdl` | Local window | Guest software renderer |
| `cocoa` | macOS window | Guest software renderer |
| `vnc` | VNC | Guest software renderer |
| `none` | No graphical output | No display backend |

`--display` changes one run. `--gl` and `--no-gl` select the GL or software member of the SDL/VNC pair. Edit `DISPLAY_MODE` in `instance.env` to keep a choice. Legacy Engine uses software rendering and its own VNC server regardless of QEMU's console display.

Virgl needs both the guest Mesa driver and a QEMU built with virglrenderer and OpenGL. If QEMU lacks the required device or display backend, the launcher prints a warning and falls back to software rendering. On Linux, use a QEMU build with `virtio-gpu-gl-pci` and `egl-headless` or SDL OpenGL support. On macOS, follow [macOS virgl](MACOS_VIRGL.md).

For JP22, set `GPU_MAX_OUTPUTS=3` in `instance.env`. Engine creates three windows and aborts if they share one EGLFS screen. QEngine supplies one connector and tablet per head; the rootfs configures matching screen names and touch-device links. Changing the screen JSON to one output does not reduce Engine's window count. Multi-head operation has only been tried on arm64.

## Changes
Use `new_instance.sh --force` to rebuild after editing a shim or builder. For manual image work, stop the guest before using `debugfs` or other filesystem tools. To install a diagnostic file while the guest runs, transfer it through the guest instead; [Remote access](REMOTE_ACCESS.md) covers that workflow. Remount `/` writable if the destination requires it.

Direct rootfs builders accept `--firmware`, `--out`, `--size` and `--force`, but do not assemble a complete instance. Their default output is the shared `build/rootfs_out.img`. Use explicit output paths and prepare the matching kernel and data disk if you run them by hand.

After booting, check the application itself, then the input or playback behavior you changed. A serial login or rendered frame does not establish those features. Record the architecture, product code, firmware version and interaction tried.

## Stopping
Run `poweroff` in the guest and wait for QEMU to exit before rebuilding or copying its disks. Stop any temporary services started for the session. Confirm that no session guests remain with `pgrep -fl qemu-system`.

[INSTANCES.md](../scripts/build_scripts/INSTANCES.md) lists launch settings and instance management. [Telemetry](BLOCKING_TELEMETRY.md) owns the blocking policy; add endpoints only to [block_telemetry.sh](../scripts/build_scripts/rootfs_steps/block_telemetry.sh).
