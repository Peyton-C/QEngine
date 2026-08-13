# Building the Image

Steps to assemble and boot an Engine OS rootfs image under QEMU: cross-compiling
the native shim tools, gathering prebuilt ARM packages, preparing disk images,
and bringing the guest up.

## Target architectures

| Arch | SoC | Controllers | Status |
|------|-----|-------------|--------|
| armv7 (armhf) | RK3288 | Prime 2/4/4+/GO/GO+, SC5000(M), SC6000(M), SC Live 2/4, Mixstream Pro/Pro+/Pro GO | Documented below (Engine 4.3.0 and earlier); Engine 5.0.4 blocked — see [Engine 5.0.4 on armv7](#engine-504-on-armv7-rk3288--in-progress-blocked) |
| arm64 (aarch64) | RK3588 | RANE SYSTEM ONE | Boots Engine with working display, touch, and external media — see [arm64 / RK3588](#arm64--rk3588-rane-system-one) |

Everything below (Docker `--platform linux/arm/v7`, `debian:bullseye` armhf
packages, `qemu-system-arm`) is the armv7/RK3288 path. It won't carry over to
RK3588 as-is.

## Prerequisites
- Docker, for cross-compiling armhf binaries under emulation
- QEMU (`qemu-system-arm`)
- A source Engine OS rootfs image (extracted from official firmware)

## 1. Cross-compiling native tools
The project's native C tools (`touchsim`, `dtshim`, `vnctouchbridge`,
`crashhandler`) are small and dependency-free — compiled directly for armhf
inside an emulated container rather than via a full cross-toolchain install:

```sh
docker run --rm --platform linux/arm/v7 -v <source_dir>:/src debian:bullseye \
  bash -c "apt-get update -qq && apt-get install -y -qq gcc libc6-dev && \
           gcc -o /src/<output> /src/<source>.c -ldl"
```

- `--platform linux/arm/v7` runs the container under QEMU user-mode emulation,
  so `gcc` inside it is a native armhf compiler — no cross-compile flags needed.
- `debian:bullseye` (Debian 11) specifically — see
  [Package / glibc version constraints](#package--glibc-version-constraints).
- Output lands directly in the mounted source directory.

## 2. Getting prebuilt ARM packages
For anything not worth compiling from source — kernel modules, or a whole
library stack like Mesa/EGL/DRM — extract the actual `.deb` for the right
architecture/version instead:

```sh
# Resolve the dependency tree and download via apt in an armhf container
docker run --rm --platform linux/arm/v7 debian:bullseye \
  apt-get install --download-only <package>

# Extract on macOS (no dpkg-deb available) — a .deb is a plain ar archive
ar x package_armhf.deb                 # -> debian-binary, control.tar.xz, data.tar.zst
tar xf data.tar.zst -C extracted/      # actual files, under extracted/usr, extracted/lib, etc.
```

Kernel modules must match the exact kernel QEMU boots, not a generic distro
kernel — pull them from that kernel's own `linux-modules-*.deb`.

### Package / glibc version constraints
Two things need to line up for a prebuilt `.so` to work once substituted into
Engine OS's userland:

1. **Architecture** — armhf (32-bit ARMv7 hard-float), matching the rk3288 target.
2. **glibc/libstdc++ generation** — a binary built against a *newer* glibc than
   the target fails to load (`GLIBC_2.35 not found`, etc). Older-than-target
   is fine; symbol versioning is backwards compatible in that direction.

Debian 11 (bullseye) ships an old enough glibc/libstdc++ generation to link
cleanly against Engine OS's userland, while still containing a working Mesa
(20.3.5) with a virtio-gpu DRI driver — the reason it's the default source for
anything compiled or extracted for the guest.

### Replacing the Mali GPU driver with Mesa
Real hardware ships a proprietary `libmali.so.14.0`, which only works against
actual Mali GPU hardware and does nothing under QEMU. Substitute a full
Mesa/EGL/GLES/DRM/X11 stack (Debian 11 armhf packages) system-wide instead, so
`virtio-gpu-pci` renders via Mesa's `virtio_gpu_dri.so`/`swrast_dri.so`.

A companion `libmali_shim.so` stub (compiled from `mali_shim.c`) — empty aside
from linking against real EGL/GLES symbols — satisfies anything that
`dlopen`s "the Mali driver" by name, transparently redirecting it to Mesa.

## 3. Preparing the eMMC overlay image
`emmc.img` backs the `/media/az01-internal` mount that Engine's systemd units
overlay `/etc` and `/var` onto. It's OS-level plumbing, not tied to a specific
Engine version — build once, reuse across every version:

```bash
parted /dev/<device> mklabel gpt
parted /dev/<device> mkpart primary ext4 1MiB 100%
sgdisk --partition-guid=1:931ad49d-ad59-0849-833a-9bf00af5b60e /dev/<device>
mkfs.ext4 /dev/<device>p1
mount /dev/<device>p1 /media/az01-internal
mkdir -p /media/az01-internal/system/etc/{overlay,.work}
mkdir -p /media/az01-internal/system/var/{overlay,.work}
umount /media/az01-internal
```

The PARTUUID must match whatever the target rootfs's fstab/systemd mount
units expect to find by UUID.

## 4. Resizing an extracted rootfs
Rootfs partitions extracted from firmware images are undersized for runtime
use (logs/cache/user data need headroom). Resize the image and grow the
filesystem to fill it:

```bash
cp rootfs_extracted.img rootfs_out.img
qemu-img resize -f raw rootfs_out.img 9114222592   # ~8.5 GiB — match a known-working size
e2fsck -f -y rootfs_out.img                        # required before resize2fs
resize2fs rootfs_out.img
```

## 5. QEMU launch
Key devices:

| Device | Purpose |
|---|---|
| `virtio-gpu-pci` | GPU, driven by Mesa (see above) |
| `usb-ehci` / `qemu-xhci` | USB controllers, for passthrough hardware |
| `usb-kbd` / `usb-tablet` | Synthetic keyboard/mouse input |
| `virtio-blk-device` | Rootfs disk |
| `sdhci-pci` + `sd-card` | eMMC overlay disk |
| `virtio-net-pci` | Networking (`hostfwd` for SSH/VNC/etc port mapping) |

```bash
#!/bin/bash
exec qemu-system-arm \
  -machine virt -cpu cortex-a15 -m 2048 -smp 4 \
  -device virtio-gpu-pci -display gtk \
  -device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet \
  -kernel <path-to-vmlinuz> -initrd <path-to-initrd> \
  -drive if=none,file=<path-to-rootfs.img>,format=raw,id=hd \
  -device virtio-blk-device,drive=hd \
  -drive if=none,file=<path-to-emmc.img>,format=raw,id=emmc \
  -device sdhci-pci -device sd-card,drive=emmc \
  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::5901-:5900 \
  -device virtio-net-pci,netdev=net0 \
  -serial stdio \
  -append "root=/dev/vda rw rootwait console=ttyAMA0"
```

This maps to `scripts/build_images.sh`/`scripts/boot_engine.sh`.

**Never run two QEMU instances against the same `emmc.img` or rootfs image
concurrently** — both are plain ext4 images with no coordination between
writers, and concurrent access (including a host loop-mount alongside a live
QEMU instance) will corrupt the filesystem. Per-instance directories give each
VM its own disks and refuse a second boot of the same instance, which is the
supported way to run more than one at a time — see
[INSTANCES.md](../scripts/build_scripts/INSTANCES.md).

## 6. Populating shim files into the rootfs
Copy the following into a new rootfs image, matching the running kernel
exactly for the `.ko` files:

- `dtshim.so`, `crashhandler.so` — `LD_PRELOAD` shims (devicetree spoofing,
  `/dev/mem` faking, crash backtraces)
- `touchsim` — uinput-based virtual touchscreen
- `snd-seq-device.ko`, `snd-seq.ko`, `snd-seq-dummy.ko` — ALSA sequencer modules
- `fake-dt/inmusic,product-code`, `serial-number`, `inmusic,az01-pcb-rev` —
  fake devicetree property files
- `fake-dev-mem` — sparse file faking `/dev/mem` for the hardware anti-clone check

```bash
sudo mount -o loop,ro rootfs_known_good.img /mnt/src
sudo mount -o loop rootfs_out.img /mnt/dst
sudo cp -a /mnt/src/root/{dtshim.so,crashhandler.so,touchsim,fake-dt,fake-dev-mem} /mnt/dst/root/
sudo cp -a /mnt/src/root/snd-seq*.ko /mnt/dst/root/
sudo umount /mnt/src /mnt/dst
```

### Swapping in a newer Engine/Qt build
`/usr/Engine` and `/usr/qt` can be swapped from a different firmware
extraction (back up originals rather than deleting):

```bash
sudo mv /mnt/dst/usr/Engine /mnt/dst/usr/Engine.bak
sudo cp -a /mnt/newer/usr/Engine /mnt/dst/usr/Engine
sudo cp -n -a /mnt/newer/usr/lib/. /mnt/dst/usr/lib/   # merge new shared libs, don't overwrite existing
```

A newer Engine binary may need a newer `libstdc++`/glibc generation than the
base rootfs provides (e.g. `GLIBCXX_3.4.29/30`, `CXXABI_1.3.13`,
`GLIBC_2.35`). Grafting the base toolchain across that gap risks breaking
other version-matched pieces (Mesa/DRI, etc) — booting the newer rootfs as
its own standalone environment is the safer path. Check exact dependencies
with `readelf -d <binary> | grep NEEDED`.

### Resetting the root password
```bash
sudo sed -i 's|^root:[^:]*:|root::|' /mnt/dst/etc/shadow
```
(`/etc` is an overlayfs at runtime — editing the base image's `/etc/shadow`
takes effect as long as the overlay's writable layer has no override.)

## 7. First boot
```bash
mount -o remount,rw /
systemctl stop engine.service
insmod /root/snd-seq-device.ko
insmod /root/snd-seq.ko
insmod /root/snd-seq-dummy.ko
printf JC11 > /root/fake-dt/inmusic,product-code   # product spoof — see ENGINEOS.md
```

Start the virtual touchscreen, then Engine:

```bash
mkfifo /root/touchfifo
/root/touchsim < /root/touchfifo &
exec 3>/root/touchfifo
cat /proc/bus/input/devices | grep -A8 TouchSim   # find which /dev/input/eventN it got

QT_QPA_PLATFORM=vnc:size=1280x720 \
QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/eventN \
LD_PRELOAD=/root/dtshim.so:/root/crashhandler.so \
LD_LIBRARY_PATH=/usr/qt/lib \
/usr/Engine/Engine -d0 > /root/engine.log 2>&1 &
```

Send a synthetic tap: `echo "tap X Y HOLD_MS" >&3`. Match the VNC framebuffer
`size=` to the target device's real panel resolution or rendering stretches.

## 8. `vnctouchbridge` — real mouse-to-touch input
A standalone RFB (VNC) proxy sitting between a real VNC client and Engine's
own `QVncServer`, translating `PointerEvent` messages into synthetic touch via
`/dev/uinput` (button-mask transitions map to touch-down/move/up). Everything
else is relayed byte-for-byte. The uinput device is created once, upfront, at
a fixed size, so its `/dev/input/eventN` path is known before Engine starts.

```bash
/root/vnctouchbridge <listen_port> <upstream_host> <upstream_port> <width> <height>
# e.g.
/root/vnctouchbridge 5902 127.0.0.1 5900 1280 720
```

Launch Engine pointed at the bridge's input device, then connect a VNC client
to the bridge's port (needs its own `hostfwd` rule) rather than Engine's own
VNC port. This lets a normal mouse drive Engine's touch-only UI interactively.

## arm64 / RK3588 (RANE SYSTEM ONE)

RANE SYSTEM ONE ships Engine OS 4.6.0 on RK3588 — the first **arm64**
Engine OS device found; every other supported controller is armv7/RK3288
(see the [Emulated Controllers](README.md#emulated-controllers) table).
Its firmware is also signed (rsa2048-signed U-Boot FIT images), which
isn't itself new — several armv7/RK3288 controllers already ship signed
firmware per that same table (Prime 4+, Prime GO+, SC Live 2/4, Mixstream
Pro+/Pro GO) — but `mpcimg` (the extraction tool used elsewhere in this
project) doesn't understand signed images. `binwalk` does, by pure
signature-scanning rather than parsing the firmware format, so it works
regardless of the signature — the same technique should apply to any of
the other signed armv7 devices too, not just this one. Extracting a
`*-Update.img` with `binwalk` recovers, among other things, three raw
ext2/ext4 filesystem images (the actual rootfs, ~830MB, plus two smaller
boot-slot partitions) and two `kernel.fit` files (redundant A/B boot
slots) — signed U-Boot FIT containers bundling the kernel, initrd, and
devicetree together.

This device's rootfs and boot chain differ from the RK3288 lineup in enough
ways that almost none of the steps above carry over unmodified. What
follows is a from-scratch recipe, confirmed working through the real
`/usr/Engine/Engine` binary launching and rendering to the screen — see
[Status](#status) at the end.

### Why the extracted kernel can't be used to boot QEMU

The signed `kernel.fit` contains a real kernel (`6.12.55-imb-2025-10-24-rt13`,
PREEMPT_RT) built narrowly for the physical RK3588 SoC. Checking its
`modules.builtin` and `.ko` tree confirms it has **no `virtio_blk`,
`virtio_gpu`, `virtio_pci`, `virtio_mmio`, or PL011/GICv3 support at all** —
none of which exist on real hardware, all of which QEMU's `virt` machine
requires. Pointing `-kernel` at it is a dead end.

The split is the same as the armv7 path: the vendor kernel/FIT is useful for
what it *tells us* (product identity, module ABI baseline, real boot
command line), not as something to actually boot. The generic kernel does
the booting; the rootfs — Engine, Qt, systemd units — is the actual prize,
same as before.

### 1. Extracting kernel + initrd + devicetree from the signed FIT

Use `dumpimage` (Fedora/Debian package `uboot-tools`/`u-boot-tools`) rather
than hand-parsing FIT offsets — it understands the external-data alignment
that plain offset arithmetic on the FIT's own properties gets subtly wrong:

```sh
dumpimage -l kernel.fit                       # list images/configs first
dumpimage -T flat_dt -p 0 -o Image       kernel.fit   # kernel (arm64 boot Image)
dumpimage -T flat_dt -p 1 -o initrd.img  kernel.fit   # ramdisk (zstd-compressed cpio)
dumpimage -T flat_dt -p 2 -o system.dtb  kernel.fit   # real hardware devicetree
dumpimage -T flat_dt -p 3 -o cmdline.dtb kernel.fit   # /chosen bootargs, as a tiny FDT overlay
```

`system.dtb` (real hardware, not for QEMU — see below) decompiles cleanly
with `dtc -I dtb -O dts` and is where the product identity comes from:

```
compatible = "inmusic,rmz2", "inmusic,az04", "rockchip,rk3588";
model = "Rane SYSTEM ONE";
inmusic,product-code = "RMZ2";
```

`RMZ2` is System One's product-code value — the arm64 equivalent of
`JC11`/`JP11`. (No `serial-number` or PCB-rev property exists in this
devicetree, unlike the RK3288 lineup — System One likely provisions that
per-unit data on the `/factory` partition instead; see
[ENGINEOS.md](ENGINEOS.md).) `cmdline.dtb` decompiles to the real boot
command line, which matters for the next section:

```
bootargs = "root=/dev/dm-0 rootwait=5 ro rfkill.default_state=0
  dm-mod.waitfor=/dev/mmcblk0p10 dm-mod.create=\"rootfs,,,ro,0 1640488 verity
  1 /dev/mmcblk0p10 /dev/mmcblk0p10 1024 1024 820244 820244 sha256 <hash>
  <hash>\" systemd.getty_auto=n fsck.repair=yes"
```

Root boots read-only through **dm-verity** on real hardware. That verity
hash is only valid for the untouched partition — the moment the rootfs is
resized or has shim files copied in (both required, same as the RK3288
path), the hash no longer matches. There's no point trying to satisfy
verity under QEMU; just don't pass any of this — override the command line
entirely (see [4. QEMU launch](#4-qemu-launch)), same as the armv7 setup
already does.

`system.dtb` itself also shouldn't be passed to QEMU. It describes real
RK3588 peripherals (real UART/GIC/storage controllers), none of which the
`virt` machine provides — same reasoning as the RK3288 path never using the
real `rk3288-az01-*.dts` either. Let `-machine virt` synthesize its own
devicetree; the real one exists only as a source of product-identity values
to spoof via a `dtshim`-style `LD_PRELOAD` (see [Status](#status)).

### 2. A generic kernel that actually boots under QEMU

Since the vendor kernel is a non-starter, use a stock distro kernel the same
way the armv7 setup uses Ubuntu 24.04's armhf cloud kernel — just the arm64
variant:

```sh
curl -LO https://cloud-images.ubuntu.com/releases/noble/release/unpacked/ubuntu-24.04-server-cloudimg-arm64-vmlinuz-generic
curl -LO https://cloud-images.ubuntu.com/releases/noble/release/unpacked/ubuntu-24.04-server-cloudimg-arm64-initrd-generic
```

`qemu-system-aarch64 -kernel` loads the gzip-compressed `vmlinuz` directly —
no need to gunzip it first. This kernel ships `virtio_blk`/`virtio_net`/etc.
built in or as initrd modules and boots straight to the real rootfs via
`switch_root`, same handoff shape as the armv7 Ubuntu initrd.

The real vendor `.ko` files (ALSA sequencer modules etc., same category
BUILDING.md's armv7 steps already reuse from a known-good image) are tied
to `6.12.55-imb-2025-10-24-rt13` specifically and won't load on this
generic kernel's different version — building/sourcing matching modules for
whatever generic kernel gets used is still an open item, same category of
problem the armv7 runbook already documents for its own module set.

### 3. `/data` and `/factory`: no eMMC overlay this time

The armv7 devices overlay `/etc` and `/var` from a single `emmc.img`
partition (see [3. Preparing the eMMC overlay image](#3-preparing-the-emmc-overlay-image)).
RMZ2 doesn't do this at all — there's no overlay unit anywhere in its
systemd tree. Instead `/etc/fstab` mounts root plain `ro`, and separate
data partitions are mounted individually by **PARTUUID**, each with a
`ConditionPathExists`-style oneshot service that auto-formats the partition
on first boot if it isn't already a valid filesystem:

```
# usr/lib/systemd/system/data.mount
[Mount]
What=PARTUUID=d6a62570-4c37-4a42-ae77-8f45bcbfda65
Where=/data
Requires=az0x-data-mkfs.service   # mkfs.ext4 -O encrypt, only if not already labeled

# usr/lib/systemd/system/factory.mount
[Mount]
What=PARTUUID=f2a055c0-1536-5020-a0c9-3944f89ba52b
Where=/factory
Requires=az0x-factory-mkfs.service
```

Without a device at those exact PARTUUIDs, systemd times out waiting for
them (~90s), which cascades into `/var/lib` (bind-mounted from
`/data/system/var-lib`) failing too. That alone doesn't crash anything —
but `/usr/Engine/Scripts/engine` calls `encrypt-fs.sh` on every launch to
`fscryptctl set_policy` two directories under `/data`; when `/data` isn't
mounted (root is `ro`, so `mkdir -p /data/downloads` fails outright), that
script's failure path falls through to a bare `systemctl reboot` — which is
exactly what a QEMU boot with no matching partition does: reaches
Multi-User target, Engine starts, then the whole VM reboots itself a couple
seconds later, forever.

Fix: build a small GPT-partitioned disk image with partitions at exactly
those two PARTUUIDs, and attach it as a second `virtio-blk` device. Leave
both partitions unformatted — `az0x-data-mkfs`/`az0x-factory-mkfs` format
them automatically on first boot:

```sh
qemu-img create -f raw data_disk.img 4G
parted -s data_disk.img mklabel gpt
parted -s data_disk.img mkpart data ext4 1MiB 2049MiB
parted -s data_disk.img mkpart factory ext4 2049MiB 100%
sgdisk --partition-guid=1:d6a62570-4c37-4a42-ae77-8f45bcbfda65 data_disk.img
sgdisk --partition-guid=2:f2a055c0-1536-5020-a0c9-3944f89ba52b data_disk.img
```

(`secure-media` and `content` are also empty top-level directories in the
extracted rootfs with no corresponding static `.mount` unit found so far —
they didn't block boot in testing; revisit if something later needs them.)

### 4. QEMU launch

**Recommended: a native arm64 host (e.g. Apple Silicon) via HVF.** Since the
guest is aarch64, an aarch64 host can run it with hardware-assisted
virtualization instead of TCG software emulation — a much bigger win than
anything GPU-specific, because it speeds up *all* CPU-side execution
(Engine's own C++/QML logic, the guest kernel, everything), not just
rendering. See [build/run-system1.sh](../build/run-system1.sh):

```bash
#!/bin/bash
exec qemu-system-aarch64 \
  -machine virt,highmem=on -accel hvf -cpu host -m 8192 -smp 8 \
  -device virtio-gpu-pci,edid=off,xres=1280,yres=800 \
  -device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet \
  -kernel vmlinuz-generic-arm64 \
  -initrd initrd-generic-arm64 \
  -drive if=none,file=rootfs_out.img,format=raw,id=hd \
  -device virtio-blk-device,drive=hd \
  -drive if=none,file=data_disk.img,format=raw,id=data \
  -device virtio-blk-device,drive=data \
  -netdev user,id=net0,hostfwd=tcp::2224-:22 -device virtio-net-pci,netdev=net0 \
  -vnc :1 \
  -serial mon:stdio \
  -append "root=UUID=<rootfs-ext-uuid> rw rootwait console=ttyAMA0"
```

No virgl needed to get a dramatic win here — Homebrew's macOS QEMU build has
no virgl/GL-accelerated `virtio-gpu` support at all (`-display` only lists
`none`/`curses`/`cocoa`/`dbus`, no `egl-headless`; no `virtio-gpu-gl-pci`
device), so rendering still falls back to software (`llvmpipe`) same as
without virgl on Linux. But since the CPU itself is no longer
software-emulated, that software rendering runs at *native* speed instead
of underneath a second layer of instruction translation — the difference
is not subtle (interactive, "nearly realtime" by feel, vs. 15+ second
input-to-response lag under plain TCG). `-cpu host` (not `-cpu max`, which
is a TCG-oriented catch-all) passes through the actual host CPU under HVF.

Moving hosts/QEMU builds resurfaces both gotchas already described in
[Status](#status) below, worth checking first if things crash on a new
host even though the exact same `rootfs_out.img` worked elsewhere:
- **IRQ numbers in `/root/fake-dt/interrupts` drift with the real device
  topology**, which can differ across QEMU builds/versions even with an
  identical command line. Symptom: `Failed to set CPU affinity for IRQ
  ttyS0 to CPU 4` (or similar) crash-looping `engine.service`. Fix: compare
  against the real `/proc/interrupts`, remap to a currently-writable `Edge`
  IRQ (see [shims/rk3588/dtshim/fake-dt-rmz2/README.md](../shims/rk3588/dtshim/fake-dt-rmz2/README.md)).
- **Compressed kernel modules silently fail to load** if this rootfs's
  `kmod` lacks `zstd` support (`modprobe --version` shows `-ZSTD`) — first
  found blocking `nls_iso8859-1` (external media wouldn't mount), but hit
  again moving to this Mac's QEMU/kernel pairing for `hid`/`hid-generic`/
  `usbhid` (`lsmod | grep hid` empty, no `/dev/input/eventN` for the USB
  keyboard/tablet at all despite `dmesg` showing them enumerate fine at the
  USB level — `usbhid` never claimed them). Same fix each time: `zstd -d
  foo.ko.zst -o foo.ko`, delete the `.zst`, `depmod -a`, `modprobe foo`.
  Worth checking `lsmod`/the relevant `/dev` or `/proc` node any time a
  module-backed feature doesn't work for no obvious reason.

**Alternative: an x86_64 host via TCG, optionally with virgl.** Without
native arm64 hardware, `-cpu max` (not a specific Cortex model — QEMU has
no discrete `cortex-a76`/`cortex-a55` model to match RK3588's big.LITTLE
cores; `max` exposes the broadest feature set TCG can emulate) is the
fallback, and it's slow enough (15+ second input latency observed) that
GPU offload via virgl is worth the extra setup even though it only
addresses rendering, not the rest of TCG's overhead:

```bash
#!/bin/bash
exec qemu-system-aarch64 \
  -machine virt -cpu max -m 8192 -smp 8 \
  -device virtio-gpu-gl-pci,edid=off,xres=1280,yres=800 \
  -display egl-headless,rendernode=/dev/dri/renderD128 \
  -device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet \
  -kernel vmlinuz-generic-arm64 \
  -initrd initrd-generic-arm64 \
  -drive if=none,file=rootfs_out.img,format=raw,id=hd \
  -device virtio-blk-device,drive=hd \
  -drive if=none,file=data_disk.img,format=raw,id=data \
  -device virtio-blk-device,drive=data \
  -netdev user,id=net0,hostfwd=tcp::2224-:22 -device virtio-net-pci,netdev=net0 \
  -vnc :1 \
  -serial mon:stdio \
  -append "root=UUID=<rootfs-ext-uuid> rw rootwait console=ttyAMA0"
```

A few more things that bit during bring-up, worth calling out directly:

- **`-smp 8` / `-m 8192`**, to roughly match a 2025/2026 8-core RK3588
  product rather than arbitrarily reusing the RK3288 lineup's 4-core/4GB
  numbers — not derived from a real System One teardown, just a sanity
  floor.
- **Identify root by filesystem UUID, not `/dev/vdX`.** With two
  `virtio-blk-device` instances attached, which one the kernel enumerates
  as `vda` vs `vdb` is not reliably the command-line order — in testing it
  came up backwards (`vda` = the data disk, `vdb` = rootfs) and
  `root=/dev/vda` silently mounted the wrong disk (a wholesale unformatted
  GPT-partitioned disk, so it just failed to mount at all, dropping to an
  initramfs shell). `root=UUID=...` sidesteps the ordering question
  entirely. Get the rootfs's UUID with `file rootfs_out.img` (ext2/3/4
  images print their UUID directly) before building the image, or `blkid`
  on it afterward.
- **`edid=off,xres=,yres=`** pins the reported mode to a fixed resolution —
  with the default `edid=on`, the advertised mode tracks the connected VNC
  client's window geometry, which is unstable and not what a real
  fixed-panel controller looks like anyway. `1280x800`, not the earlier
  `1920x1080` used during initial display bring-up: Engine's own UI scales
  oddly at 1080p, and 1280x800 is also meaningfully cheaper to render — see
  the `virtio-gpu-gl-pci` note below for why render cost matters here.
- **`virtio-gpu-gl-pci` + `-display egl-headless,rendernode=...`**, not
  plain `virtio-gpu-pci` — offloads rendering to the host's real GPU via
  virgl instead of software-rendering inside the guest. Without this, Mesa
  falls back to `llvmpipe` (`MESA-LOADER: failed to open virtio_gpu: ...
  No such file or directory` in Engine's log is the tell), which means
  every QML frame is software-rasterized *inside* QEMU's own software
  (TCG) aarch64 CPU emulation — a software rasterizer running on a
  software CPU, compounding into severe input-to-response latency (15+
  seconds observed). This vendor rootfs's Mesa "megadriver" install
  (`/usr/lib/dri/*_dri.so`, all hardlinks of one file — see
  [3.](#3-data-and-factory-no-emmc-overlay-this-time)'s sibling section on
  Panthor) was never built with a `virtio_gpu`/virgl Gallium driver at all,
  since real System One hardware never needs one — confirmed by the file
  simply not existing under any of the 39 aliased names. Hardlinking
  `virtio_gpu_dri.so` to one of the *existing* names (e.g. `swrast_dri.so`)
  does **not** work and is worse than doing nothing: it satisfies the
  `dlopen()` by filename but the underlying binary has no virtio_gpu
  driver personality compiled in, so it loads and then fails
  (`MESA-LOADER: driver exports no extensions ((null))`), and — because
  the kernel already negotiated `+virgl` at the DRM level by this point —
  `eglfs` no longer has the graceful software-only fallback it used
  without GL support, so Engine hard-aborts every launch instead of just
  running unaccelerated.

  The actual fix: pull a **real** virtio_gpu/virgl-capable `_dri.so` from
  Debian bookworm's own distro-packaged Mesa (22.3.6, arm64 — same
  cross-compile container already used for shims,
  `apt-get install libgl1-mesa-dri`) and drop *only that one file* in as
  `/usr/lib/dri/virtio_gpu_dri.so`, replacing whatever's there. This works
  as a single-file swap — confirmed no `libglvnd` anywhere in this rootfs
  (`libEGL.so.1`/`libgbm.so.1` sit directly in `/usr/lib/`, i.e. classic
  Mesa loading, not vendor-neutral dispatch), so the vendor's own
  `libEGL`/`libgbm` just `dlopen()`s whatever `_dri.so` matches the
  requested driver name via the standard, fairly version-stable DRI driver
  ABI — no need to replace the higher-level libraries too, and no need to
  cross-compile Mesa from source. Not committed to this repo (23MB,
  foreign-origin binary — regenerate with the recipe above rather than
  vendoring it) but freely redistributable (Debian's Mesa build is
  MIT/GPL). Confirmed working: no `MESA-LOADER` errors, no aborts, the
  atomic-commit shim's `MODESET`/`FLIP` calls keep succeeding unchanged
  (the KMS-level pixel-format/atomic-commit fixes are orthogonal to which
  Mesa driver renders the frame), and — most importantly — dramatically
  faster perceived input latency, confirmed by hand over a real VNC
  client. Still not fast — TCG's own instruction-emulation overhead for
  everything else (Engine's C++/QML logic, not just rendering) is a
  separate cost this doesn't touch — but no longer double-software-limited.

  One cost: **`screendump` (the HMP command used throughout this doc and
  in `BUILDING.md`'s testing) stops working** once `-display egl-headless`
  is in use (`Error: no surface` — that display backend doesn't populate
  the legacy surface `screendump` reads from). Use a real VNC client
  against `-vnc :1` instead to actually see the screen.
- **`usb-tablet`**, not `usb-mouse` — QEMU's virtual tablet reports
  absolute coordinates, which is what a touchscreen needs (`usb-mouse`
  reports relative motion, unusable for tap-to-position input). Engine's
  QML doesn't respond to this device directly, though — see
  [Status](#status) for the synthetic-touch bridge needed on top of it.
- **`-serial mon:stdio`** multiplexes the guest serial console and QEMU's
  own HMP monitor onto the same terminal — Ctrl-A then `c` toggles between
  them. Useful for `screendump` (grab a PPM screenshot of the emulated
  display) without needing a separate VNC client.
- **To test external media**, attach a FAT32/exFAT-formatted image through
  the existing `qemu-xhci` controller rather than another
  `virtio-blk-device` — a real `usb-storage` device is a closer stand-in
  for an actual USB port: `-drive if=none,file=usb_test.img,format=raw,
  id=usbtest -device usb-storage,drive=usbtest,bus=xhci.0`. See
  [Status](#status) for a kernel-module gotcha this hits.

### 5. Resizing the rootfs, and a mount-free way to blank the root password

[4. Resizing an extracted rootfs](#4-resizing-an-extracted-rootfs) applies
unchanged — `qemu-img resize` + `e2fsck -f` + `resize2fs` on a copy of the
extracted ext2 image.

For small edits like blanking `/etc/shadow`'s root hash, `debugfs` can
read *and write* an ext2/3/4 image directly, entirely without loop-mounting
(so no root/`sudo` needed at all — useful on a host where `sudo` isn't
passwordless):

```sh
debugfs -R "cat /etc/shadow" rootfs_out.img > /tmp/shadow.orig
sed 's|^root:\*:|root::|' /tmp/shadow.orig > /tmp/shadow.new
debugfs -w -R "rm /etc/shadow" rootfs_out.img
debugfs -w -R "write /tmp/shadow.new /etc/shadow" rootfs_out.img
```

### 6. Toolchain for cross-compiling shims

The rootfs's own `libc.so.6` reports **glibc 2.39** — noticeably newer than
the RK3288 lineup, and past what Debian 11 (bullseye, used for the armv7
shims — see [Package / glibc version constraints](#package--glibc-version-constraints))
provides. Debian 12 (bookworm, glibc 2.36) arm64 is the equivalent safe
choice here — older than the target, which is the direction that's fine;
avoid anything glibc 2.40+ (e.g. Debian 13/trixie) since that would go the
unsafe direction. Not yet exercised end-to-end (see [Status](#status)).

### Status

Confirmed working, in order: signed-FIT extraction → generic aarch64
kernel boot → root mounts by UUID → `/data`/`/factory` auto-format and
mount → full systemd boot to a login prompt → root login (password
blanked per above) → `engine.service` actually execs
`/usr/Engine/Engine` → **real Qt/QML startup** (product identity resolved,
crash reporter/sentry init, timezone detection, MIDI device scan, EQ
preset defaults, EDisks hotplug handling for the attached virtio disks).

Getting from "Engine execs" to "real Qt/QML startup" took two rounds of
the same `dtshim`-style fix, both landing in
[shims/rk3588/dtshim/dtshim_rmz2.c](../shims/rk3588/dtshim/dtshim_rmz2.c) (cross-compiled
in a Debian 12 arm64 `podman`/`docker` container per
[6.](#6-toolchain-for-cross-compiling-shims) above — `docker` itself
needs a daemon that isn't always running/passwordless-startable; rootless
`podman run --platform linux/arm64 ...` works as a drop-in substitute):

1. **Product identity crash** (`air.planck.config: Unable to find product
   "" in config map!`, `SIGABRT`) — `/sys/firmware/devicetree/base/inmusic,product-code`
   doesn't exist under QEMU's synthesized devicetree. Fixed the same way
   as the RK3288 devices: `dtshim_rmz2.c` remaps that path (plus
   `serial-number` and the `dsi@fde20000/panel@0/rotation` path — see
   [1.](#1-extracting-kernel--initrd--devicetree-from-the-signed-fit) for
   where those values came from) to files under `/root/fake-dt/`. Fake
   files and the updated shim source live in
   [shims/rk3588/dtshim/fake-dt-rmz2/](../shims/rk3588/dtshim/fake-dt-rmz2/).
2. **Hardware IRQ-affinity crashes** — separately, Engine itself
   (compiled in, not a shell script) hard-throws
   (`std::runtime_error`, uncaught, aborts) if it can't find a
   `/proc/interrupts` line by name for six real-hardware components —
   `dwc3`, `fe210000.sata`, `fea10000.dma-controller`, `ff0c0000.dwmmc`,
   `ff0f0000.dwmmc`, `ttyS0` — none of which exist under QEMU. Fixed by
   also remapping `/proc/interrupts` to a fake file
   ([shims/rk3588/dtshim/fake-dt-rmz2/interrupts](../shims/rk3588/dtshim/fake-dt-rmz2/interrupts))
   containing all six names, each reusing a real IRQ number already
   present in the guest — necessary because after finding each IRQ,
   Engine immediately writes its CPU affinity, which needs the number to
   resolve to a real `/proc/irq/<N>/` directory or that write fails too
   (uncaught, also fatal). A GPIO-controller IRQ was tried first and
   rejected outright by the kernel (`EIO` on the affinity write); the
   GIC/MSI-routed virtio IRQs accept it fine.

Deployed via an `engine.service` systemd drop-in
(`/etc/systemd/system/engine.service.d/override.conf`, setting
`Environment=LD_PRELOAD=/root/dtshim_rmz2.so:/root/drmatomic_rmz2.so` and
`Environment=QT_QPA_PLATFORM=eglfs`) rather than editing the vendor
`runengine`/`engine` scripts in place — see
[shims/rk3588/dtshim/fake-dt-rmz2/README.md](../shims/rk3588/dtshim/fake-dt-rmz2/README.md)
for the exact deployment steps and a couple of file-format gotchas
(`printf` vs. plain file copy for the product-code/serial-number values;
`rotation`'s raw big-endian binary cell).

**Display: working.** This build is Qt **6.7.2**, not Qt 5.15.2, and its
`plugins/platforms/` has no `libqvnc.so` at all — Qt dropped the VNC QPA
backend, so the existing `QT_QPA_PLATFORM=vnc` + `vnctouchbridge`
remote-display strategy this whole project is built around doesn't carry
over here. What *is* present: `eglfs` with KMS/GBM integration, and the
real GPU driver is the open-source, mainlined **Panthor** (Mali-G610/
Valhall) — no proprietary `libmali.so` blob to substitute for at all,
unlike RK3288.

`eglfs` alone produced a black screen — Qt's `eglfs-kms-gbm` backend calls
the legacy `DRM_IOCTL_MODE_SETCRTC`/`DRM_IOCTL_MODE_PAGE_FLIP` ioctls, and
both returned `EINVAL` under `virtio-gpu-pci`. Two theories that looked
promising were both ruled out by direct testing: the submitted modeline
wasn't actually mismatched against the connector's own advertised mode
(byte-for-byte identical, still rejected), and skipping `SETCRTC` outright
to test whether the CRTC might already be active from boot didn't help
either. The actual root cause, found by re-implementing the modeset as a
real `DRM_IOCTL_MODE_ATOMIC` commit (see below) and turning on
`drm.debug=0x3ff` (`echo 0x3ff > /sys/module/drm/parameters/debug`) to get
the kernel's atomic-check rejection reason logged to `dmesg`:

```
[drm:drm_atomic_plane_check] [PLANE:31:plane-0] invalid pixel format AR24 little-endian (0x34325241), modifier 0x0
[drm:drm_atomic_check_only] [PLANE:31:plane-0] atomic core check failed
```

`virtio-gpu`'s primary plane in this QEMU/kernel combination rejects
`ARGB8888` (`AR24`) outright — Qt allocates its scanout framebuffer with
an alpha channel, and the plane only accepts opaque `XRGB8888` (`XR24`).
Both formats have an identical memory layout (the alpha byte is simply
unused in `XRGB8888`), so this is a one-field fix, not a real
incompatibility. This explains why the failure was identical whether
`SETCRTC` was legacy or hand-rolled atomic — the framebuffer itself was
never valid for that plane, regardless of which ioctl path submitted it.

The fix, in [shims/rk3588/dtshim/drmatomic_rmz2.c](../shims/rk3588/dtshim/drmatomic_rmz2.c):
intercepts `DRM_IOCTL_MODE_ADDFB2` and rewrites `pixel_format` from
`ARGB8888` to `XRGB8888` before it reaches the kernel, **and** separately
replaces Qt's legacy `SETCRTC`/`PAGE_FLIP` calls with a real
`DRM_IOCTL_MODE_ATOMIC` commit (resolving the primary plane, its CRTC,
connector, and all the property IDs involved via
`DRM_IOCTL_MODE_OBJ_GETPROPERTIES`/`GETPROPERTY`, then submitting a proper
modeset commit with `DRM_MODE_ATOMIC_ALLOW_MODESET` and flip commits with
`DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`). The pixel-format
fix alone would likely have been sufficient — the legacy path probably
would have started working too — but the atomic commit was kept
regardless since it's a more direct, more robust primitive than continuing
to depend on however Qt's legacy call happens to be shaped in this
specific Qt build, especially since this project intentionally tracks an
older, non-latest Engine OS release. Also requires
`DRM_CLIENT_CAP_ATOMIC` set on the fd (`DRM_IOCTL_SET_CLIENT_CAP`) before
the kernel will accept an atomic commit or return primary/cursor planes
from `GETPLANERESOURCES` at all — Qt never sets this itself since it only
uses the legacy path, so the shim sets it lazily on first use.

Confirmed with a QEMU `screendump`: Engine's onboarding screen (headphones
logo, "engine dj" wordmark, "Next" button) renders correctly.

**External media (USB/SD): working**, unlike the armv7/4.3.0 lineup.
Engine's own `edisksd` hotplug scanner runs and correctly enumerates any
attached block device that isn't the boot disk as removable media — proven
by attaching a `data_disk.img` ext4 partition, which `edisksd` picked up
immediately and Engine's own UI correctly flagged with an "Incompatible
Format... reformat to exFAT or FAT32" dialog (i.e. the detection path
works; ext4 specifically isn't accepted, matching real hardware). Testing
with an actual FAT32-formatted drive — `-device usb-storage,drive=...,
bus=xhci.0` attached through the existing `qemu-xhci` controller (a
faithful stand-in for a real USB port, more so than another
`virtio-blk-device`), `mkfs.vfat -F 32` — first hit a separate wall: the
kernel logged `FAT-fs (sda): IO charset iso8859-1 not found` and never
mounted it. Root cause: this kernel's `nls_iso8859-1.ko` (and other
modules) ship `zstd`-compressed (`.ko.zst`), and the guest's `kmod`
(`modprobe --version` reports `-ZSTD`) was built **without** zstd support,
so it can't decompress *any* compressed module — a systemic gap, not
specific to this one module. Worked around by decompressing the module by
hand (`zstd -d nls_iso8859-1.ko.zst -o nls_iso8859-1.ko`, `rm` the `.zst`,
`depmod -a`) so plain `modprobe`/kernel auto-loading can find it
uncompressed. After that, restarting `engine.service` picked up `/dev/sda`
cleanly — `EDisks filesystem added`, mounted, no format complaint. Worth
checking whether other auto-loaded modules hit the same silent failure
elsewhere.

**Touch input: working.** Clicking through `-vnc :1` (or QEMU's own
`mouse_move`/`mouse_button` HMP commands) reaches the guest as absolute
pointer events on `/dev/input/event2` (`QEMU QEMU USB Tablet`, attached via
`usb-tablet` — see [4. QEMU launch](#4-qemu-launch)). udev correctly
classifies it `ID_INPUT_MOUSE=1`, and Qt's built-in `evdevmouse` handler
does open and consume it (confirmed via `QT_LOGGING_RULES=qt.qpa.input=true`
— "Adding mouse at /dev/input/event2", "create mouse handler..."). But
clicks through that path never produced any visible effect in Engine's UI,
even on an always-present, unambiguous target (the "Incompatible Format"
dialog's "Ok" button) — while keyboard input (`sendkey ret` dismissing the
same dialog's default button) worked immediately. That asymmetry, plus
`libqevdevtouchplugin.so` existing in this build alongside evdevmouse,
points at this DJ touchscreen UI's QML wiring genuine touch semantics
(`TapHandler`/`MultiPointTouchArea`) rather than mouse clicks — exactly the
reason the RK3288 lineup's `vnctouchbridge` exists at all. The root cause
was never fully pinned down architecturally beyond that; empirically,
switching to synthesized touch fixed it immediately.

Root architectural difference from RK3288 to note before reusing that
project's approach directly: `vnctouchbridge` is a full RFB proxy that
parses Engine's *own* VNC server protocol (Qt5's `vnc` QPA plugin) to
intercept `PointerEvent` messages, because on RK3288 Engine itself is the
VNC server. Here Engine uses `eglfs` (direct KMS/DRM scanout, no VNC QPA
plugin at all — see [Qt6, and no VNC QPA plugin](ENGINEOS.md#qt6-and-no-vnc-qpa-plugin))
and *QEMU* is the VNC server, already injecting real client clicks into
the emulated `usb-tablet` as normal kernel evdev events. So there's no RFB
protocol to parse at all — [shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.c](../shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.c)
is a much smaller program that just reads `/dev/input/event2` directly and
re-emits it as a synthetic multitouch device via `uinput`
(`ABS_MT_SLOT`/`TRACKING_ID`/`POSITION_X`/`Y` protocol B + `BTN_TOUCH`,
`INPUT_PROP_DIRECT` so udev tags it `ID_INPUT_TOUCHSCREEN=1`), reusing only
the uinput setup code from `vnctouchbridge.c`. It also `EVIOCGRAB`s the
source device so Qt's own now-inert mouse handler for it goes quiet, and
reads the source device's real `ABS_X`/`ABS_Y` range via `EVIOCGABS` at
startup rather than assuming QEMU's current `0..32767` — self-adjusting if
that ever changes, not hardcoded.

Deployed as a systemd service
([shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.service](../shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.service))
ordered before `engine.service` (`Before=`/`WantedBy=multi-user.target` on
the bridge; `After=touchbridge_rmz2.service` +
`Requires=touchbridge_rmz2.service` added to `engine.service.d/override.conf`)
— required because Qt's `evdevtouch` device discovery only scans for
touch-capable devices once at its own startup, so the synthetic
touchscreen has to already exist before Engine launches, not just before
the first real touch input arrives. Confirmed working by watching the
"Incompatible Format" dialog's `Ok` button (for `/data`, then `factory` —
same disks flagged in the external-media testing above, cycling back as
`edisksd` periodically rescans, not a bug) actually dismiss and reveal the
*next* queued dialog on each simulated tap, proven again by the user
directly over a real VNC client connection. Perceptibly slow in practice — almost certainly `MESA-LOADER: failed to
open virtio_gpu` (noted above) forcing all QML compositing through
software rendering (`llvmpipe`) on top of QEMU's own software (TCG) aarch64
emulation, not anything touch-specific, since the bridge itself is a
trivial blocking read/write loop with no added latency of its own.

**Performance: solved by running on native arm64 hardware instead of
chasing GPU offload further.** The same `rootfs_out.img` — no shim changes,
same touch bridge, same atomic-commit/pixel-format fixes — boots natively
on Apple Silicon via HVF (see [4. QEMU launch](#4-qemu-launch)) and reaches
the *actual* Engine collection browser (real demo tracks, deck UI, track
detail views, all touch-navigable) at what the user described as "nearly
realtime" — a different experience entirely from the multi-second
input lag on TCG, virgl or not. This ended up mattering more than the
virgl work above: virgl only ever addressed rendering, while HVF removes
software emulation from *all* guest CPU execution. The two gotchas that
resurfaced moving to this host (IRQ drift, compressed kernel modules
silently failing under a `kmod` without zstd support) are documented in
[4. QEMU launch](#4-qemu-launch) rather than repeated here — both are
generic to "moved to a different QEMU build/host," not new bugs.

Also worth noting for whoever picks this back up: the shim files above
were pushed into the *running* guest live (`wget` from an HTTP server on
the host, per [build/REMOTE_ACCESS.md](../build/REMOTE_ACCESS.md)'s workflow)
for fast iteration, not baked into `rootfs_out.img` on disk — and
separately, `debugfs -w` was used at one point to inject files into that
same image file *while QEMU still had it open* for the live boot test.
That's exactly the concurrent-writer scenario
[5. QEMU launch](#5-qemu-launch)'s eMMC warning (and this section's own
[3.](#3-data-and-factory-no-emmc-overlay-this-time)) says never to do —
harmless by luck this time since nothing on the live guest side happened
to touch the same blocks, but run `e2fsck -f` on `rootfs_out.img` before
trusting it for a from-scratch boot, and prefer the live-guest-`wget`
approach (or a clean shutdown first) over `debugfs -w` on an
already-booted image going forward.

### Engine 5.0.4

The hypothesis going in: the base OS's own `VERSION_ID` (`/usr/lib/os-release`
— `az04`, `5.0.14 (scarthgap)`) was already in the 5.x line even on the
firmware packaged as "Engine 4.6.0", and InMusic was unlikely to maintain
Engine against two different Qt major versions at once — so 5.0.4 (current
latest, `SYSTEMONE-5.0.4-Update.img`) was expected to need little beyond
what 4.6.0 already required. Confirmed on every count after extracting it
with the same recipe as [1.](#1-extracting-kernel--initrd--devicetree-from-the-signed-fit)
(same `rk3588-az04-rmz2` FIT description, signed with the same `rane-1`
key): identical Qt **6.7.2** (`libqeglfs.so` present, still no `libqvnc.so`),
identical `engine.service` (`Type=forking`, same `runengine`/`cleanup`
scripts), and identical `/data`/`/factory` `PARTUUID`s
(`d6a62570-...`/`f2a055c0-...`) — meaning the *existing* `data_disk.img`
from the 4.6.0 setup is reusable as-is, no rebuild needed.

Brought up with **zero shim changes**: the exact same
`dtshim_rmz2.so`/`drmatomic.so`/`touchbridge_rmz2` binaries and
`fake-dt-rmz2` files from the working 4.6.0 setup, copied unmodified into a
fresh 5.0.4 rootfs. For a transplant this size (several files across
`/root`, `/etc/systemd/system/`, `/usr/Engine/ScreenConfiguration/`),
bulk file operations in a real Linux environment beat repeating
`debugfs -w` one file at a time: a `--privileged` Docker container (Docker
Desktop's Linux VM has real loop-device support) with both rootfs images
bind-mounted, `losetup` + `mount` on each individually (mounting two loop
devices in one `mount -o loop` invocation was unreliable — attach each
explicitly with `losetup -f` first), then plain `cp -a`. Confirmed working
after that: display renders (a visibly refreshed onboarding UI — different
background/button layout from 4.6.0, but through the identical `eglfs` +
atomic-commit-shim path), and touch works, confirmed directly by hand over
a real VNC client.

One more thing this surfaced: **the kernel-module gap (kmod lacking zstd,
[4.](#4-qemu-launch)) is not a rootfs property and does not persist across
a real reboot.** `/lib/modules/6.8.0-136-generic/` isn't present on either
rootfs image on disk at all (confirmed by loop-mounting and checking
directly) — it's provided fresh at boot time (almost certainly unpacked by
`initrd-generic-arm64`, which both this and the 4.6.0 rootfs share
unmodified), still zstd-compressed, every single boot. So the
`nls_iso8859-1`/`hid`/`hid-generic`/`usbhid` decompress-and-`depmod` fix
has to be re-run after *every* fresh VM boot — not a one-time fix baked
into the image, regardless of which Engine version or host it's paired
with. Worth fixing in the initrd itself at some point rather than
repeating by hand each time.

> **Since done.** `get_arm64_kernel.sh` builds the initrd with its own curated
> `MODULES` list plus a `copymods` `init-bottom` hook that relocates the initrd's
> modules onto a tmpfs on the real root, so a current build needs no per-boot
> manual step. The rest of this entry describes the older hand-run setup.

New files: `build/run-system1-504.sh` (host launch script, identical to
`run-system1.sh` apart from paths/UUID/ports). That per-version copying is
superseded by [INSTANCES.md](../scripts/build_scripts/INSTANCES.md): the
launchers now take their paths, ports and root UUID from the environment, so one
script serves every version.
The 5.0.4 rootfs/data-disk images themselves aren't committed (same reason
`rootfs_out.img` isn't — large, regeneratable from the recipe above).

### Audio playback: working — build and launch requirements

Full mechanism, root cause and the corrections it forced on earlier findings
are in [ENGINEOS.md](ENGINEOS.md#audio-playback-working--the-real-gate-and-corrections-to-the-sections-below).
This section is only what you have to *do* to get sound out of a build.

**1. Attach the sound card playback-only.** `-device hda-output`, never
`-device hda-duplex`:

```
-device ich9-intel-hda -device hda-output,audiodev=mac -audiodev coreaudio,id=mac
```

With a capture PCM present, Engine makes the capture device its default and
leaves the playback slot unassigned, then drives capture only — playback sits
in `XRUN` forever with `Audio_probe` frozen, and no error is printed. All of
[scripts/qemu/](../scripts/qemu/)'s launch scripts already do this; the
Linux-targeted ones use `pipewire` rather than the macOS-only `coreaudio`.

**2. Preload `alsashim_rmz2.so`.** Built and installed by
[build_arm64_rootfs.sh](../scripts/build_scripts/build_arm64_rootfs.sh), which
adds it to `engine.service`'s `LD_PRELOAD` and sets `ALSASHIM_CARD=0`.
Without it Engine rejects the emulated card on its *name* before ever
touching its PCMs, and there is no configuration route around that — the
allowlist is compiled into `Engine.bin`.

**3. Playback control comes from the virtual control surface**, which the
build installs and enables as a service. SYSTEM ONE's transport buttons are
physical, so nothing on the touchscreen can start a deck. On an image built
by the script this needs no setup — `midisurface_rmz2.service` starts before
Engine, answers the identity handshake, and disables motorized mode by
itself. To drive it, write commands to its fifo:

```sh
echo 'play left'  > /run/midisurface_rmz2.fifo
echo 'cue right'  > /run/midisurface_rmz2.fifo
echo 'press 0x0F 0x05' > /run/midisurface_rmz2.fifo   # Browse button
```

The fifo lives in `/run`, not `/root`: this rootfs mounts `/` read-only, so
creating it under `/root` fails at boot and the service restart-loops. That
bug was invisible during interactive testing because the rootfs had been
manually remounted read-write — a hazard worth remembering for anything
validated by hand in this guest.

Running it manually instead (stop the service first — it holds the ALSA
client name):

```sh
systemctl stop midisurface_rmz2
mkfifo /tmp/midififo
/root/midisurface_rmz2 RMZ2_Controller --motor-off < /tmp/midififo &
exec 3>/tmp/midififo
# Engine must be (re)started while the surface already exists: its MIDI
# enumerator binds devices at startup and won't pick one up later.
systemctl restart engine.service
echo 'play left' >&3
```

The motor-off is not optional. SYSTEM ONE's platters are motorized and its
decks wait on platter timecode that cannot exist under emulation, so play
silently does nothing until motorized mode is toggled off — while cue still
previews audio, which makes cue a useful signal source when checking whether
audio output itself is alive. Engine does not persist the setting and always
starts motorized, so `--motor-off` re-applies it on each binding. It fires on
Engine's identity inquiry (i.e. exactly when Engine has bound the surface, so
it re-arms across Engine restarts on its own) and is debounced, because the
control is a *toggle* and Engine sends the inquiry more than once per
startup — acting on each would turn the motor straight back on.

#### Driving a real USB controller

Engine only binds a control surface that answers its inMusic identity
inquiry, which no third-party controller does, and this rootfs's
`KnownDevices` table has exactly one entry that reaches Engine's decks. So a
generic controller cannot be bound directly at all. The arrangement that
does work:

```
real controller --> midisurface_rmz2 --forward --> Engine
                    (answers the handshake, relays MIDI unchanged)
```

with the *mapping* supplied by the assignment QML Engine loads, swapped per
controller by [controllermap.sh](../shims/rk3588/controllermap/controllermap.sh):

```sh
controllermap.sh --list        # connected vid:pid ids + recognised mappings
controllermap.sh --dry-run     # what would be installed, without doing it
controllermap.sh               # install the match (runs at boot as a service)
controllermap.sh --restore     # back to the vendor RMZ2 mapping
```

Manifest lines are whitespace-separated, `<vid:pid> <mapping-dir>
[description]`, and the mapping directory holds files under exactly the names
Engine resolves from `KnownDevices`:

```
mappings/<your-controller>/RMZ2_Controller_Assignments.qml   required
mappings/<your-controller>/RMZ2_Controller_Device.qml        optional
```

The directory name is arbitrary (it just has to match the manifest); the
filenames are not. `Device.qml` is only needed to change how Engine talks
*to* the surface — SysEx identity, LED/pad-display encoding, the motor
commands — as opposed to what the controls mean.

Installs always start from a `*.vendor` snapshot taken on first run, so runs
are idempotent and switching controllers cannot compound edits; if no listed
controller is attached the vendor mapping is restored, since a mapping for
absent hardware is worse than none.

Authoring a mapping means copying the vendor
`RMZ2_Controller_Assignments.qml` and changing the numbers — the structure
(which QML component provides which function) stays. RMZ2's defaults are
listed in [ENGINEOS.md](ENGINEOS.md#audio-playback-working--the-real-gate-and-corrections-to-the-sections-below);
the decks are on MIDI channels 0x04/0x05, mixer channels 0x00/0x01, global
0x0F.

Four things that constrain what is mappable:

- **Only Engine's own QML vocabulary exists** (`PlayCue`, `Sync`,
  `MixerChannelCore`, `ActionPads`, ...). A control with no counterpart has
  nowhere to map to.
- **Relative encoders may need code, not configuration.** The QML names a CC;
  it cannot reinterpret its values. Controllers differ (two's complement,
  offset binary, absolute), so a browse knob that scrolls backwards, too
  fast, or one-way needs translation in the forwarder.
- **The pitch fader is 14-bit** (`ccUpper`/`ccLower`). A 7-bit controller can
  drive `ccUpper` alone, at lower resolution.
- **LED/display feedback will not work.** Engine emits RMZ2's own protocol
  (it pushes cue-point names to the pad displays, for instance) and a foreign
  controller won't understand it. Harmless, but expect dark buttons.

Note also that `loadNote` in the deck model is vestigial — nothing references
it. On SYSTEM ONE, loading a track is the deck's browse-encoder *push*
(`pushNote`), and both decks' encoders use identical numbers, distinguished
only by MIDI channel.

#### The tty1 getty is disabled

Engine renders fullscreen via eglfs/KMS on the same VT the console getty
lives on, and the getty keeps reading the keyboard underneath it — so every
keystroke reaches *both* Engine and an invisible root login shell. Typing
into Engine's search box also types into that shell, and it is entirely
possible to power the machine off by accident that way (observed).

The build removes the enablement symlink and masks both `getty@tty1` and
`autovt@tty1` (`autovt@` is an alias of `getty@` that logind spawns on VT
allocation, so disabling alone is not enough). `serial-getty@ttyAMA0` is left
alone and remains the way in over `-serial stdio`. Engine's keyboard input is
unaffected, since eglfs reads evdev directly rather than through the VT.

#### The shims are built from source now

Every shim binary is `.gitignored` (`*.so`, plus `touchbridge_rmz2` by name),
so a fresh clone has sources only.
[build_arm64_rootfs.sh](../scripts/build_scripts/build_arm64_rootfs.sh) builds
all five — `dtshim_rmz2.so`, `drmatomic_rmz2.so`, `alsashim_rmz2.so`,
`touchbridge_rmz2`, `midisurface_rmz2` — in one `debian:bookworm` arm64
container before installing them. Previously it copied artifacts that nothing
produced, which worked only if a previous session had left them in the tree.

`libdrm-dev` (for `drmatomic`) and `libasound2-dev` (for `midisurface`) are
installed in that container; `alsashim` needs neither, since it declares the
two libasound types it touches and resolves the real symbols via `dlsym`.

#### Diagnostic logging is off by default

Two shims used to log unconditionally, and both were expensive enough to be
felt:

- `drmatomic_rmz2.so` logged every atomic commit — i.e. a synchronous write
  to the journal on *every rendered frame*. Now behind `DRMATOMIC_DEBUG`;
  modesets and failures still log, since those are rare and useful.
- `dtshim_rmz2.so`'s devicetree-access log (added to rule the devicetree out
  of the audio investigation, which it did) took a mutex and did a separate
  `fopen`/`fprintf`/`fclose` per matching read, and `serial-number` is re-read
  dozens of times a session. Now behind `DTSHIM_DT_LOG`.

Turning Engine's own `QT_LOGGING_RULES` up is similarly costly — useful while
debugging, worth removing afterwards.

#### Reading Engine's own audio enumeration

The single most useful diagnostic here, and how the card-name allowlist was
found:

```
Environment=QT_LOGGING_RULES=air.devicemanager.*=true
```

A rejected card logs `Get card info for hw:N ...` and then stops. An accepted
one goes on to `Query device 0 ...` / `Device name hw:N`. Other useful
categories: `air.assignments*` and `air.deviceidentifier*` (which dumps the
whole `KnownDevices` table, including the identity pattern a control surface
must match).

#### Static-analysis tooling

Three scripts in `build/ghidra/` (gitignored, alongside the Ghidra project and
`Engine.bin`), reusable well beyond this investigation:

- `rtti_graph.py` — parses the Itanium C++ RTTI graph straight out of the
  binary and prints real class hierarchies. This is what showed `ALSADevice`
  genuinely derives from `airAudioDevice`, overturning the decompilation-based
  conclusion that it didn't. No decompiler needed.
- `vtables.py` — recovers vtables and maps any `FUN_xxxxxx` address to its
  owning class and slot, which turns anonymous decompiler output into named
  methods. This is what identified `FUN_0181fdc0` as `ALSACombinedDevice`'s,
  not `airHost`'s.
- `decomp.py` — decompiles by address, by string reference, or lists xrefs,
  reusing the already-analyzed project via PyGhidra (`analyze=False`) instead
  of repeating the ~5.5 minute auto-analysis.

Ghidra loads `Engine.bin` at image base `0x100000`, so addresses recorded in
these docs are file offsets + `0x100000`.

### Audio playback: decompiling Engine.bin

With display/touch/MIDI all working (see [ENGINEOS.md](ENGINEOS.md#arm64--rk3588-rane-system-one)),
audio did not play. It does now — see
[Audio playback: working](#audio-playback-working--build-and-launch-requirements)
above for the requirements and
[ENGINEOS.md](ENGINEOS.md#audio-playback-working--the-real-gate-and-corrections-to-the-sections-below)
for the mechanism. The section below records the decompilation effort that
preceded that finding, and reached some conclusions the final answer
overturns. This section covers the two
QEMU/toolchain-specific parts of that investigation: why the real onboard
codec driver can't be reproduced here, and the Ghidra headless setup used
to decompile the stripped `Engine.bin` to reach that conclusion.

Migrated the whole VM off the Mac (`hvf`) onto a Radxa Dragon Q6A SBC
(`kvm`) partway through this investigation — macOS's `usb-host` reliably
kernel-panicked the host the moment QEMU touched the MC6000MK2 at the
USB-audio-class level (AppleUSBAudio's driver binding never tears down
cleanly; a known QEMU-on-macOS limitation, not fixable from this side).
Linux's udev-based passthrough doesn't have that failure mode. Picked up
along the way, general-purpose rather than RMZ2-specific: KVM + `-smp 8`
at 4GB RAM (this box has 7.4GB total), `virtio-gpu-gl-pci` +
`egl-headless,rendernode=/dev/dri/renderD128` for real virgl offload
through the Adreno GPU, and `-audiodev pipewire` feeding an emulated
`ich9-intel-hda`+`hda-duplex` card out to the host's PipeWire sink. See
[build/run-system1-504-radxa.sh](../build/run-system1-504-radxa.sh).

#### The `az04-codec` driver exists, and is unusable here

The real devicetree (`build/systemone_linux_debive_tree.dtb`, extracted
per [1.](#1-extracting-kernel--initrd--devicetree-from-the-signed-fit))
defines a `simple-audio-card` named literally `"RMZ2"`, backed by an
`az04-codec` node (`compatible = "inmusic,az04-codec"`,
`inmusic,capture-channels`/`playback-channels = <0x06>` — a real
6-channel DJ-mixer codec, master/booth/headphone). Tempting hypothesis:
Engine expects an ALSA card named `RMZ2` specifically. Tested directly —
`rmmod snd_hda_intel && modprobe snd_hda_intel id=RMZ2` renames the
emulated card's ALSA id, confirmed via `/proc/asound/cards` — and it made
no difference; Engine still logged the failure with an empty device
name, not `"RMZ2"`. (Decompilation later explained why: the real gate is
a C++ `dynamic_cast` type check, not a name lookup — see ENGINEOS.md.)

The matching kernel module does genuinely exist. Binwalk-extracting
`SYSTEMONE-4.6.0-Update.img` recovers the real ~867MB vendor rootfs
(alongside two smaller boot-slot partitions) as a plain ext2/ext4 image —
readable directly with `debugfs -R` without mounting or root:

```sh
debugfs -R 'ls -l /lib/modules' vendor_rootfs.img
#   6.12.55-imb-2025-10-24-rt13
debugfs -R 'cat /lib/modules/6.12.55-imb-2025-10-24-rt13/modules.dep' vendor_rootfs.img | grep az04
#   kernel/sound/soc/codecs/snd-soc-inmusic-az04.ko:
debugfs -R 'dump /lib/modules/.../snd-soc-inmusic-az04.ko /tmp/az04.ko' vendor_rootfs.img
strings /tmp/az04.ko | grep -iE 'vermagic|description|author|alias'
#   description=inMusic AZ04 ASoC codec driver
#   author=John Keeping <jkeeping@inmusicbrands.com>
#   alias=of:N*T*Cinmusic,az04-codecC*
#   vermagic=6.12.55-imb-2025-10-24-rt13 SMP preempt mod_unload aarch64
```

Confirmed real, confirmed matching the devicetree's `compatible` string
via its OF alias — and confirmed unusable here for two independent,
structural reasons, not one:

1. **Kernel ABI.** `vermagic` pins it to the vendor's exact
   `6.12.55-imb-2025-10-24-rt13` **PREEMPT_RT** build. It won't load into
   the generic Ubuntu kernel QEMU boots (see
   [2.](#2-a-generic-kernel-that-actually-boots-under-qemu)) without
   `insmod --force`, and RT kernels change core locking/scheduling
   structures enough that a forced load is more likely to oops than
   work — not attempted, given point 2 makes it moot regardless.
2. **No hardware for it to bind to, even if loaded.** The codec driver is
   only half of the `simple-audio-card` DAI link; the other half
   (`sound-dai` pointing at the SoC's I2S/TDM controller) is real RK3588
   silicon that QEMU's `virt` machine doesn't emulate at all. The card
   can never finish probing regardless of whether the codec side loads.

Same category of wall as [Why the extracted kernel can't be used to boot
QEMU](#why-the-extracted-kernel-cant-be-used-to-boot-qemu) above — a
genuine "QEMU doesn't emulate this SoC's IP block" gap, not something a
`dtshim`-style spoof can paper over the way product-identity/DRM were.

#### Ghidra headless: `linux_arm_64` has no decompiler, `.py` postscripts need PyGhidra

Reaching the `airHost::updateAudioDeviceChanged` finding in ENGINEOS.md
required decompiling the stripped `Engine.bin` (`ELF ... aarch64 ...
stripped`, `.dynsym` present, no `.symtab`). Two unrelated version/platform
gotchas hit along the way, both worth recording since they'll recur for
any future decompilation work on this project's arm64 targets:

1. **The Radxa (Linux/aarch64) host can't run Ghidra's decompiler at
   all.** `ghidra_12.1.2_PUBLIC/Ghidra/Features/Decompiler/os/` ships
   prebuilt native `decompile` binaries for `linux_x86_64`, `mac_arm_64`,
   `mac_x86_64`, `win_x86_64` — **not** `linux_arm_64`. Every
   `decompileFunction()` call fails with `os/linux_arm_64/decompile does
   not exist`, even after a full, successful auto-analysis pass (the
   string/xref search parts are pure Java and work fine; only actual
   decompilation needs the missing native). No native build available
   without compiling Ghidra's C++ decompiler from source. Fix: run the
   same analysis on the Mac instead (`mac_arm_64` is bundled) — copy
   `Engine.bin` over (`scp`), reuse the same script.
2. **Ghidra 12.1.2 dropped bundled Jython for `.py` postscripts.**
   `analyzeHeadless ... -postScript foo.py` now fails with `Ghidra was
   not started with PyGhidra. Python is not available`, even though the
   expensive auto-analysis phase completes and saves normally — only the
   postscript step is affected. Fix: `pip install pyghidra` (needs a
   venv on macOS due to PEP 668 — Homebrew's Python blocks unmanaged
   global installs) and call `pyghidra.open_program(binary_path,
   project_location=..., project_name=..., analyze=False,
   nested_project_location=False)` directly instead of going through
   `analyzeHeadless -postScript` — `analyze=False` reuses the
   already-saved, already-analyzed project instead of repeating the ~5.5
   minute auto-analysis pass. `nested_project_location=False` matters
   because `analyzeHeadless`-created projects are flat
   (`project/Name.gpr`), not nested (`project/Name/Name.gpr`, PyGhidra's
   own default layout) — get this wrong and it silently starts a fresh
   project instead of reopening the analyzed one. The old
   `FlatProgramAPI` calls (`getReferencesTo`, `getFunctionContaining`,
   `DecompInterface`) are otherwise unchanged from classic Jython
   scripting.

### Reimplementing `az04-codec` as a loadable kernel module

Follow-up to [Audio playback: decompiling
Engine.bin](#audio-playback-decompiling-enginebin) — full narrative and
result in
[ENGINEOS.md#audio-playback-a-real-alsa-card-reached-and-opened](ENGINEOS.md#audio-playback-a-real-alsa-card-reached-and-opened).
This section covers the toolchain and live-deployment mechanics; sources
live in [shims/rk3588/az04-audio/](../shims/rk3588/az04-audio/)
(`az04_codec.c`, `az04_card.c`, `Makefile`).

#### Getting a kernel module toolchain that exactly matches the running kernel

The project's kernel/initrd are pulled from Debian trixie's
`linux-image-arm64` package
([get_arm64_kernel.sh](../scripts/build_scripts/get_arm64_kernel.sh)),
currently `6.12.101+deb13-arm64`. Building an out-of-tree module that
actually loads needs an *exact* vermagic match (`CONFIG_MODVERSIONS` is
on — even a matching kernel version with different symbol CRCs fails to
load), so the build container has to be the same package at the same
point in time, not just "a Debian trixie image":

```sh
docker run -d --name az04dev --platform linux/arm64 debian:trixie sleep infinity
docker exec az04dev bash -c "
  apt-get update -qq &&
  apt-get install -y -qq linux-image-arm64 linux-headers-arm64 build-essential"
```

`linux-headers-arm64`'s `/lib/modules/<kver>/build` symlink points at
`/usr/src/linux-headers-<kver>` — a normal, ready-to-use
`make -C $KDIR M=$PWD modules` target, no kernel source tree needed.
`linux-image-arm64` (installed alongside, not otherwise used for
building) is where the *pool* of real Debian-built `.ko`s lives —
needed here for `snd-soc-core.ko` and its own dependencies
(`snd-pcm-dmaengine.ko`, `snd-compress.ko`), none of which are in this
project's trimmed initrd (see `MODULES=` in `get_arm64_kernel.sh` — audio
support there is deliberately curated down to USB-class/HDA only, no
ASoC at all). Grab them with `docker cp` and `xz -d`, same as any other
module in this project's build recipe.

**Do not build modules in a newer Debian release than the guest's own
glibc.** `snd-soc-core.ko` etc. are pure kernel code, ABI-locked to the
kernel version alone, so `debian:trixie` (matching exactly) is correct
and required for those. But any *userspace* diagnostic binaries pulled in
for testing (`aplay`/`amixer`/`speaker-test` from `alsa-utils`, used
below) link against glibc, and the guest's own `libc.so.6` reports
**2.39** while `debian:trixie`'s is **2.41** — newer, so guest-incompatible
(glibc symbol versioning is backwards-compatible only). Same rule as
[Toolchain for cross-compiling shims](#6-toolchain-for-cross-compiling-shims)
above: use `debian:bookworm` (glibc 2.36) for anything that needs to
actually *run* on the guest, reserving the exact-version container only
for kernel-ABI-locked `.ko` builds.

#### Live-deploying into an already-booted guest, without touching the disk image

Earlier shim iteration always happened offline — edit source, rebuild,
`debugfs -w` the `.so` directly into `rootfs_out.img`, then boot. That's
unsafe once QEMU already has the image open with the guest's ext4
mounted read-write live (confirmed directly: `debugfs -w` succeeded
without complaint, but risks corrupting a filesystem the kernel already
has its own in-memory state for). Once the VM is up, get files in over
the network instead:

```sh
python3 -m http.server 8124   # from the host, in the directory with the .ko files
```

QEMU's usermode networking (`-netdev user`, this project's default) makes
the host reachable from the guest at the fixed gateway address
`10.0.2.2` regardless of the guest's own DHCP-assigned address — no
port-forward setup needed for this direction, only `curl -o file
http://10.0.2.2:8124/file` from inside the guest.

One more gotcha specific to this arm64/RMZ2 image: `/` mounts **plain
`ro`** per `/etc/fstab` (see
[ENGINEOS.md's arm64 filesystem layout](ENGINEOS.md#filesystem-layout-differs-from-the-rk3288-overlay-scheme)) —
writing anything under `/root/` from a live shell fails with `Read-only
file system` until `mount -o remount,rw /` first. `debugfs` bypasses this
(it writes the block device directly, ignoring the live mount's state
entirely) which is exactly why it's unsafe to mix with a live guest —
the two writers have no idea about each other.

#### Debugging a silent kernel-side `EINVAL` without a rebuild

The actual `snd-soc-dummy` bug (full writeup in ENGINEOS.md) produced a
bare `EINVAL` from `aplay` with no corresponding kernel log line at all —
`dmesg` stayed completely empty across the failing `open()` call, even
right after `dmesg -C`. Two facilities together (this generic kernel
already ships `CONFIG_DYNAMIC_DEBUG`+`CONFIG_KPROBES`+`debugfs`, no
rebuild needed) got to the exact line in one pass each, cheaper than
re-deriving kernel internals from memory:

1. **Dynamic debug**, to light up existing `dev_dbg()`/`pr_debug()`
   call sites in specific source files:
   ```sh
   echo 'file soc-pcm.c +p' > /sys/kernel/debug/dynamic_debug/control
   echo 'file pcm_native.c +p' > /sys/kernel/debug/dynamic_debug/control
   # ...one line per file, matched by source filename, not full path
   ```
   This alone found the failing function's name
   (`snd_pcm_hw_constraints_complete failed`, from a
   `pcm_dbg()` one call site above the real ALSA-core function that
   contains it) — but not *which* of the several constraint calls inside
   it was the one returning negative, since only the outer wrapper had a
   trace point.
2. **A one-shot kretprobe**, to bisect further without adding a debug
   line inside every candidate function by hand:
   ```sh
   cd /sys/kernel/debug/tracing
   echo 'r:cmask snd_pcm_hw_constraint_mask $retval' > kprobe_events
   echo 'r:cmask64 snd_pcm_hw_constraint_mask64 $retval' >> kprobe_events
   echo 'r:cminmax snd_pcm_hw_constraint_minmax $retval' >> kprobe_events
   echo 'r:crules snd_pcm_hw_rule_add $retval' >> kprobe_events
   echo 1 > events/kprobes/enable
   echo > trace   # clear
   # ...trigger the failing open()...
   cat trace
   ```
   The trace showed ~20 `snd_pcm_hw_rule_add` calls (constraint-list
   init, expected), then exactly **one** `snd_pcm_hw_constraint_mask`
   call and nothing after — meaning the very first constraint check
   inside `snd_pcm_hw_constraints_complete()` (`ACCESS`) was the one
   failing, and everything downstream of it (`FORMAT`, `CHANNELS`,
   `RATE`, ...) was never even reached. That pointed straight at
   `hw.info` being `0`, and from there to `dummy_dma_open()`'s
   self-matching guard in the actual kernel source (pulled directly from
   `cdn.kernel.org` — `linux-6.12.101.tar.xz`, matching Debian's patch
   level closely enough for core ASoC/ALSA files, which Debian doesn't
   patch).

Matching real kernel source against a *specific* Debian kernel build
(rather than working purely from headers, which only give struct/macro
shapes, not implementations) was what actually closed this out — headers
alone were enough to write the two new modules, but not enough to
understand why one specific mainline helper function behaved
unexpectedly.

### A minimal native `gdb`, when `gdbserver` + remote `lldb` isn't reliable enough

Full narrative in
[ENGINEOS.md#audio-playback-live-attach-session-and-a-real-observer-effect-finding](ENGINEOS.md#audio-playback-live-attach-session-and-a-real-observer-effect-finding).
This is the toolchain recipe on its own, since it's reusable well beyond
that one investigation: `gdbserver` (small, few deps, easy to deploy —
see [Reimplementing az04-codec](#reimplementing-az04-codec-as-a-loadable-kernel-module)
above) paired with a *remote* `lldb` from the host turned out to be
fundamentally unreliable for this project's target — attach-time thread
storms (Engine runs 40-80+ threads at points) seem to desync the
gdbserver/lldb remote-protocol handshake in ways that never fully
resolved across several distinct failure modes. A real, **native**
debugger running directly on the guest (no network/remote-protocol layer
between the debugger and the target at all) is dramatically more stable
for the same target — but Debian's packaged `gdb` drags in ~56 shared
libraries (Python, ICU, Kerberos, curl, source-highlight, babeltrace...),
and a full copy of all of them onto the guest crashed with a bare `Bus
error` on `gdb --version`, before printing anything at all — not
diagnosed, not worth chasing given the fix is cheaper: build a **minimal**
`gdb` from source with the heavy optional features stripped out.

```sh
# In the debian:bookworm shim-build container (matching glibc <=
# guest's 2.39 — see "Toolchain for cross-compiling shims" above; gdb
# itself isn't kernel-ABI-locked like the .ko builds, so this container
# reuses the same version-compatibility rule, not the exact-kernel one):
apt-get install -y build-essential texinfo bison flex libncurses-dev \
  libreadline-dev zlib1g-dev libgmp-dev   # libgmp-dev: GMP is load-bearing,
                                           # cannot be configured out
wget -q https://ftp.gnu.org/gnu/gdb/gdb-13.1.tar.xz   # match `gdb --version`
tar xf gdb-13.1.tar.xz && cd gdb-13.1 && mkdir build && cd build
../configure --disable-nls --without-python --without-guile \
  --without-babeltrace --without-debuginfod --disable-source-highlight \
  --without-lzma --without-libunwind-ia64 --disable-tui --without-mpfr \
  --without-expat --without-gdb-datadir-relocatable
make -j$(nproc)
strip -o /tmp/gdb-stripped gdb/gdb   # 144MB unstripped -> ~10MB
```

Cuts the dependency list from ~56 down to **5**: `libtinfo`, `libgmp`,
`libstdc++`, `libm`, `libgcc_s` — and the last three are already present
on this rootfs (Engine itself needs `libstdc++`/`libgcc_s`, and
`libc`/`libm` are the guest's own). Only `libtinfo.so.6` and
`libgmp.so.10` actually need copying over alongside the binary itself,
same `docker cp` + realpath-resolve-symlinks + `curl` pattern as every
other file transfer in this project — see
[Live-deploying into an already-booted guest](#live-deploying-into-an-already-booted-guest-without-touching-the-disk-image)
above.

`gdb`'s own `shell <cmd>` — runs a host command without detaching from
the inferior — turned out to be the single most useful feature for this
kind of session: cross-checking `journalctl` output *from inside the
same `gdb` prompt*, without ever needing to detach/reattach, was what
actually surfaced the observer-effect finding (a full-history, not just
recent-window, `journalctl` search made the difference — see
ENGINEOS.md). One gotcha: `shell` consumes the **entire rest of the
line** as its command — `shell cmd1; shell cmd2` fails (the second
`shell` gets passed literally to `/bin/sh -c "cmd1; shell cmd2"`, which
then fails with `sh: shell: not found`); chain with plain `;`/`&&` inside
a *single* `shell` invocation instead.

## Engine 5.0.4 on armv7 (RK3288) — in progress, blocked

Hypothesis going in: since RANE SYSTEM ONE's 4.6.0 firmware already shipped
a `5.0.14 (scarthgap)` base OS, and InMusic was unlikely to maintain Engine
against two different Qt majors at once, the *entire* product line —
including the older armv7/RK3288 controllers, not just arm64 — was likely
already on Qt 6/`eglfs` by the time of Engine 5.0.4. Confirmed: extracting
`PRIMEGOPLUS-5.0.4-Update.img` (signed, `denon-1` key — the first
signed-**armv7** image this project has extracted, vs. the earlier
unsigned Prime Go images and RANE's signed **arm64** image) with the same
binwalk recipe shows Qt **6.7.2**, `libqeglfs.so` present, no `libqvnc.so`
— identical finding to the arm64 side, just on RK3288. This single FIT
image/rootfs covers four device identities at once
(`rk3288-az01-jc11s`/Prime 4+, `rk3288-az05-jp11s`/Prime GO+,
`rk3288-az05-jp20`, `rk3288-az05-jp21`) — confirmed by spoofing the
product code as `JC11S` (Prime 4+) via the same `inmusic,product-code`
devicetree-property mechanism used throughout this doc, and getting a
fully-rendered "PRIME 4 PLUS" Settings UI in return, live over VNC.

**New shims** (`shims/rk3288/dtshim/dtshim_jc11s.c`,
`shims/rk3288/drmatomic_jc11s/`, `shims/rk3288/touchbridge_jc11s/`):
`drmatomic_rmz2.c` and `touchbridge_rmz2.c` from the arm64/RMZ2 work
recompile for armhf **completely unmodified** and work identically —
neither depends on CPU architecture at all, only on the Linux DRM/uinput
kernel UAPIs, which use fixed-width types specifically so 32- and 64-bit
callers are both safe. Only `dtshim_jc11s.c` needed real changes: a fresh
file (not a port of the old `shims/rk3288/dtshim/dtshim.c`, which carries
Qt5-era EGL/GBM interception hacks already known to break Qt6's own
GBM handling — see the arm64 section above) that keeps RK3288's real
devicetree paths (product-code, serial-number, rotation, `/dev/mem`,
reused unchanged from the old file, since the underlying hardware layout
doesn't change between Engine versions) plus a `/proc/interrupts` remap
carried over from the RMZ2 shim, needed here too.

**Two familiar gotchas resurfaced** immediately on first boot, both
already documented above, just with RK3288-specific specifics:
- `overlay.ko.zst` (not `nls_iso8859-1`/`hid` this time) hit the same
  kmod-without-zstd gap, breaking the `/etc` and `/var` overlayfs mounts
  entirely (`az0x-data-mkfs`/`etc.mount`/`var.mount` all fail, dropping to
  emergency mode) until manually decompressed each boot.
- Reusing the existing `emmc.img` for `/data` (its `PARTUUID`,
  `931ad49d-ad59-0849-833a-9bf00af5b60e`, matches this rootfs's
  `az0x-data-mkfs.service` exactly, so no new disk image was needed) hit
  filesystem inconsistencies from that image's long history across this
  project — `az0x-data-mkfs` runs `e2fsck` first and treats "errors
  corrected" as a hard failure rather than success, so the *first* boot
  after reusing an old `emmc.img` needs `systemctl restart
  az0x-data-mkfs.service` by hand once e2fsck has already fixed things;
  clean on every boot after that.

**New gotcha, specific to this device/OS combination**: `runengine`
auto-**powers off the whole VM** if the `Engine` binary ever exits with
code 0 without writing an expected "quit reason" marker — a real
production safety behavior (assume a silent clean exit means something's
badly wrong, not worth staying up in a broken state), but it means any
investigation here races against an automatic shutdown a few seconds after
Engine's first launch. Worked around with `systemctl mask engine.service`
before letting boot continue, then launching `/usr/Engine/Engine` by hand
from a shell with the same env vars `engine.service.d/override.conf`
would've set — the poweroff logic lives in the wrapper script, not
`engine.service` itself, so this sidesteps it entirely for interactive
debugging.

**Root cause, found via a cross-compiled `strace` (Debian bullseye ships
one for armhf directly — `apt-get install strace`, no static-build dance
needed unlike the earlier aarch64 case) tracing the manually-launched
binary**: `Engine` exits via a clean `exit_group(0)` immediately after
`Setting QT_QPA_EGLFS_KMS_CONFIG to "/tmp/ScreenConfig.json"` — no
crash, no printed error, nothing Qt-level (never gets far enough to hit
`QT_DEBUG_PLUGINS=1`/`QT_LOGGING_RULES=*=true` output at all). Two
separate things happen in that window:
1. `access("/usr/lib/qt6/plugins/egldeviceintegrations/libqeglfs-mali-integration.so", F_OK)`
   — a **hardcoded, and wrong even for real hardware**, path: the actual
   EGL device integration plugins on this rootfs live at
   `/usr/lib/plugins/egldeviceintegrations/` (`libqeglfs-kms-integration.so`,
   `libqeglfs-emu-integration.so`) with no `qt6` path segment at all,
   matching where `platforms/libqeglfs.so` also lives. Creating the
   missing directory and placing a copy of the real KMS integration `.so`
   at the expected (wrong) path makes this specific check pass
   (`access()` now returns `0`) — confirmed via a second `strace` run —
   but:
2. **Independent of that check's result**, `Engine` unconditionally opens
   `/`, `/sys`, `/sys/devices`, `/sys/devices/platform`, lists the real
   directory entries via `getdents64`, and exits right after — with or
   without the Mali file present. No matching device name turned up as a
   plain string in the binary (checked for RK3288's known real GPU
   devicetree address, `ffa30000.gpu`, among other patterns — not found),
   so this is likely walking each platform device's own attributes
   (driver binding, `modalias`, etc.) at runtime rather than checking a
   fixed name, which `strings` can't reveal.

**Not yet resolved.** Unlike every fix earlier in this doc, this one isn't
a plain file `dtshim` can redirect via `open`/`fopen` — `/sys/devices/platform`
is a real, kernel-populated directory (not a regular file), so faking an
entry in it means intercepting `getdents64()` itself and fabricating a
directory entry in the returned buffer, then potentially building out a
fake sysfs subtree underneath it if `Engine` drills further into whatever
device it's trying to find. Meaningfully more invasive than anything else
in this project so far. Deliberately stopped here rather than build that
out speculatively — worth attempting if this device/version combination
becomes a priority, but the diagnosis above (masked `engine.service` +
manual launch + cross-compiled `strace`) is the reusable part regardless
of who picks it up next.

## See also
- [ENGINEOS.md](ENGINEOS.md) — Engine OS internals, product spoofing, known limitations
- [build/REMOTE_ACCESS.md](../build/REMOTE_ACCESS.md) — driving a VM on a remote build host
