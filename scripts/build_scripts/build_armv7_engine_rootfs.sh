#!/bin/bash
# Automates extraction and modification of a stock *armv7 / RK3288* Engine OS rootfs
# for QEngine. The arm64 sibling is build_arm64_rootfs.sh; this file is a minimal
# diff against it, and the differences are all consequences of the architecture and
# of what this rootfs already ships.
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image with binwalk 3.
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block Sentry telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build the dtshim/drmatomic/touchbridge shims for armhf. Only dtshim is
#      RK3288-specific; the other two compile from the RK3588 sources unmodified.
#   5. Copy those shims + fake-dt files into /root.
#   6. Wire touchbridge.service and an engine.service.d override so
#      engine.service loads the shims and starts eglfs.
#   7. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind Engine's fullscreen display.
#
# Nothing is staged into /usr/lib — this rootfs already ships everything the
# graphics stack needs, and step 6's environment is what points Qt and Mesa at it.
# See the note above the shim install for the two things that were staged while
# that was still being worked out, and why neither is needed.
#
# Not carried over from the arm64 build: controllermap, which exists to swap a
# real USB controller's assignment files in and hardcodes RMZ2's directory.
#
# alsashim and midisurface ARE carried over, because the control surface needs
# both: midisurface is the virtual surface Engine binds, and alsashim is what
# makes Engine willing to bind it at all -- its MIDI enumerator drops any
# sequencer client with no card number, which a userspace client never has.
# Only that one gate is wanted here. alsashim's other job, getting an emulated
# sound card past Engine's card-name allowlist, is an audio concern and the
# 32-bit virt machine has no PCI for the HDA device anyway, so no ALSASHIM_CARD
# is set and audio stays out of scope.
#
# Usage: build_armv7_engine_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  *-Update.img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Environment:
#   PRODUCT_CODE  which of this image's device identities to spoof. Default JP07.
#
# Requires: binwalk (3.x), qemu-img, docker.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR_SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIMS_DIR="$REPO_ROOT/shims"
# Both builders mount the whole shims tree, so a shim shared between them is
# reached at the same path in either: /shims/<name> for the shared ones,
# /shims/rk3288 or /shims/rk3588 for the two that are genuinely per-SoC.
# Names the compiled output of the shared shims, so one source yields one
# artifact per architecture: shims/<name>/<name>_$SHIM_ARCH.
SHIM_ARCH="armhf"

OUT_PATH="$REPO_ROOT/build/rootfs_out.img"
SIZE=4294967296
FORCE=0
# Defaulted so that omitting --firmware reaches the check below instead of
# dying with `FIRMWARE_IMG: unbound variable` under `set -u`.
FIRMWARE_IMG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --firmware) FIRMWARE_IMG="$2"; shift 2 ;;
        --out) OUT_PATH="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

if [ ! -f "$FIRMWARE_IMG" ]; then
    echo "ERROR: Valid firmware image required: $FIRMWARE_IMG" >&2
    exit 1
fi

if [ -e "$OUT_PATH" ] && [ "$FORCE" -ne 1 ]; then
    echo "ERROR: $OUT_PATH already exists — refusing to overwrite (pass --force to replace it)." >&2
    exit 1
fi

for bin in binwalk qemu-img docker file; do
    command -v "$bin" >/dev/null 2>&1 || { echo "ERROR: '$bin' is required but not found on PATH." >&2; exit 1; }
done

OUT_DIR="$(cd "$(dirname "$OUT_PATH")" && pwd)"
OUT_NAME="$(basename "$OUT_PATH")"
mkdir -p "$OUT_DIR"

# Pin the host-architecture containers explicitly. Docker caches images under a
# bare tag regardless of the platform they were pulled for, so once anything has
# pulled debian:bookworm-slim for arm64 (this script's own shim container does,
# and so does the documented binfmt check), a later `docker run` with no
# --platform silently reuses the arm64 image and runs emulated. That made the
# privileged container's architecture depend on pull order rather than on intent.
case "$(uname -m)" in
    x86_64|amd64)   HOST_PLATFORM="linux/amd64" ;;
    aarch64|arm64)  HOST_PLATFORM="linux/arm64" ;;
    *)              HOST_PLATFORM="" ;;
esac

### 1. Extract the rootfs partition with binwalk ############################

# Shared with the other rootfs builder and with new_instance.sh, which needs the
# same extraction to identify a firmware's device family before it can choose
# between us. It sets EXTRACTED_ROOTFS_SIZE and cleans up its own scratch dir.
# shellcheck source=extract_rootfs.sh
. "$SCRIPT_DIR_SELF/extract_rootfs.sh"

extract_rootfs "$FIRMWARE_IMG" "$OUT_PATH"

### 2. Grow the image and filesystem #########################################

echo "--- resizing image to $SIZE bytes ---"
qemu-img resize -f raw "$OUT_PATH" "$SIZE"

### 2b. Build the shims ######################################################
# The shim binaries are .gitignored (*.so, plus the shared shims' per-arch
# outputs by name), so
# a fresh clone has sources only — this step is what makes the install step
# below work at all rather than silently depending on artifacts a previous
# session happened to leave in the working tree. Building them here also
# means an edited .c can never be shadowed by a stale .so.
#
# debian:bookworm for glibc 2.36, comfortably older than the guest's 2.39
# (older is the safe direction) — see docs/BUILDING.md's "Toolchain for
# cross-compiling shims". One container for all of them, since the apt-get
# dominates the cost.
echo "--- building shims from source ---"
# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform linux/arm/v7 debian:bookworm >/dev/null
docker run --rm --platform linux/arm/v7 \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -v "$SHIMS_DIR:/shims" \
    debian:bookworm bash -c '
        # No apostrophes below, comments included: this whole block is one
        # single-quoted argument, and one would end it early and hand the rest to
        # the host shell — which then runs the gcc lines against a /shims that does
        # not exist there. The privileged container further down takes its script
        # through a quoted heredoc instead and has no such restriction.
        set -e
        # These shims are copied straight into an armv7 rootfs, so a
        # wrong-architecture container here would graft foreign binaries in.
        case "$(uname -m)" in armv7l|armv8l|armhf) ;; *)
            echo "ERROR: shim container is $(uname -m), expected armv7l." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # libdrm-dev: drmatomic includes drm.h/drm_mode.h. No libgl1-mesa-dri:
        # unlike the RMZ2 rootfs this one already ships a complete Mesa, so
        # nothing foreign needs staging in. See the privileged container below.
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1

        # dtshim is the only genuinely RK3288-specific shim. drmatomic and
        # touchbridge build from the RK3588 sources: neither depends on the CPU
        # architecture, only on the DRM and uinput kernel UAPIs, whose fixed-width
        # types make 32- and 64-bit callers equally correct (docs/BUILDING.md, the
        # JC11S / Engine 5.0.4 section).
        #
        # What 32-bit did change is the *name* an LD_PRELOAD shim has to export.
        # The glibc in this guest is a 64-bit-time_t build, so its headers redirect
        # ioctl() to __ioctl_time64() and that is the name its libdrm imports;
        # drmatomic.c exports both, which is what makes it interpose here at
        # all. Nothing in that is RK3288-specific, so it stays in the shared source.
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/rk3288/dtshim/dtshim_jc11s.so /shims/rk3288/dtshim/dtshim.c -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /shims/drmatomic/drmatomic_$SHIM_ARCH.so /shims/drmatomic/drmatomic.c -ldl
        gcc -O2 -Wall \
            -o /shims/touchbridge/touchbridge_$SHIM_ARCH /shims/touchbridge/touchbridge.c

        # The control surface, from the same RK3588 sources: neither is
        # architecture-specific. alsashim resolves everything through dlsym so it
        # needs no ALSA headers; midisurface links libasound directly.
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/alsashim/alsashim_$SHIM_ARCH.so /shims/alsashim/alsashim.c -ldl
        gcc -O2 -Wall \
            -o /shims/midisurface/midisurface_$SHIM_ARCH /shims/midisurface/midisurface.c -lasound
    '

for artifact in rk3288/dtshim/dtshim_jc11s.so; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done
# The shared shims are checked separately: they live outside SHIMS_DIR, and each
# one is named for the architecture it was built for rather than for a device.
for artifact in alsashim/alsashim_$SHIM_ARCH.so drmatomic/drmatomic_$SHIM_ARCH.so \
                touchbridge/touchbridge_$SHIM_ARCH midisurface/midisurface_$SHIM_ARCH; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + engine.service, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-armv7-engine-rootfs-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq e2fsprogs util-linux >/dev/null 2>&1
# NOTE: this container intentionally runs the *host* architecture, not armhf —
# e2fsck/resize2fs on a multi-GB image is far slower under qemu-user emulation.
# So it must never be the source of anything that ends up inside the guest
# rootfs. Nothing here is: the shims come from the armhf container above, and the
# DRI driver is a copy of one the rootfs already ships.

IMG="/out/$OUT_NAME"

# The steps both rootfs builders share, so a change to one lands in both. Each
# file in rootfs_steps/ defines one function and explains what it is for; the
# calls below read as the sequence they are.
for _step in /steps/*.sh; do . "$_step"; done

resize_filesystem "$IMG"

echo "--- mounting via loop device ---"
LOOPDEV="$(losetup -f)"
# losetup -f asks the kernel via /dev/loop-control for the next free number, but
# the node itself only exists in this container's /dev if it already existed when
# the container started. On a host with many loops already taken (snap mounts hold
# dozens) the answer is a number above anything present, and losetup then fails
# with "No such file or directory". Create the node ourselves — we are privileged,
# loop is major 7, and the minor is the loop number.
[ -e "$LOOPDEV" ] || mknod "$LOOPDEV" b 7 "${LOOPDEV##*/loop}"
losetup "$LOOPDEV" "$IMG"
mkdir -p /mnt/rootfs
# extents/64bit are ext4 features even though `file` labels this ext2
# (no journal) — mount as ext4 so the kernel driver understands them.
mount -t ext4 "$LOOPDEV" /mnt/rootfs
cleanup() { umount /mnt/rootfs || true; losetup -d "$LOOPDEV" || true; }
trap cleanup EXIT

# Guard against firmware from the wrong device family. Otherwise this build
# completes happily and the guest panics ~45s into boot with an opaque
# "request_module: modprobe binfmt-464c" as run-init fails to exec an /sbin/init of
# the wrong architecture. The dynamic loader's filename is architecture-specific,
# so its presence is an unambiguous check.
if [ ! -e /mnt/rootfs/lib/ld-linux-armhf.so.3 ]; then
    echo "ERROR: this firmware's rootfs is not 32-bit ARM (no /lib/ld-linux-armhf.so.3)." >&2
    echo "       This builds armv7/RK3288 Engine OS firmware only." >&2
    exit 1
fi
if [ ! -d /mnt/rootfs/usr/Engine ]; then
    echo "ERROR: no /usr/Engine in this rootfs, so it is not an Engine OS image." >&2
    exit 1
fi

harden_for_emulation /mnt/rootfs
skip_firmware_update /mnt/rootfs

# Nothing is staged into /usr/lib/dri. Unlike the RMZ2 rootfs, this one ships a
# complete Mesa — /usr/lib/dri holds ~35 *_dri.so entries that are all the same 13MB
# gallium megadriver — and MESA_LOADER_DRIVER_OVERRIDE=kms_swrast in the unit below
# names one that is already there. An earlier version of this script also copied that
# megadriver to virtio_gpu_dri.so, the name Mesa's loader would look for from the
# kernel's device name; with the override set, that name is never used.
#
# Nor is anything staged at Engine's hardcoded Mali eglfs-integration path,
# /usr/lib/qt6/plugins/egldeviceintegrations/libqeglfs-mali-integration.so, which
# Engine access()es and which is wrong even on real hardware (the real plugins live
# in /usr/lib/plugins/egldeviceintegrations, no qt6 segment). Satisfying it was
# tried, and Engine renders with the path absent: the failure it was suspected of
# causing was really Qt picking no device integration at all, which
# QT_QPA_EGLFS_INTEGRATION now settles.

echo "--- inserting shims into /root ---"
cp -a /shims/rk3288/dtshim/dtshim_jc11s.so /mnt/rootfs/root/dtshim.so
cp -a /shims/drmatomic/drmatomic_$SHIM_ARCH.so /mnt/rootfs/root/drmatomic.so
cp -a /shims/touchbridge/touchbridge_$SHIM_ARCH /mnt/rootfs/root/touchbridge
# Landed without a device in the name, unlike the shims either builder has
# carried since before this build existed. alsashim is architecture-neutral --
# it resolves everything through dlsym -- so both builders will eventually
# install one file from one source, and the guest-side path is the half of that
# which costs nothing to settle now.
cp -a /shims/alsashim/alsashim_$SHIM_ARCH.so /mnt/rootfs/root/alsashim.so
# A service rather than a preload: it is a MIDI device Engine binds, not a
# library Engine loads. It reads /root/fake-dt/inmusic,product-code itself to
# decide which device to answer Engine's inquiry as, so one binary and one unit
# serve every product this image can be built as.
cp -a /shims/midisurface/midisurface_$SHIM_ARCH /mnt/rootfs/root/midisurface
chmod 755 /mnt/rootfs/root/dtshim.so /mnt/rootfs/root/drmatomic.so \
          /mnt/rootfs/root/touchbridge /mnt/rootfs/root/alsashim.so \
          /mnt/rootfs/root/midisurface

# The devicetree properties dtshim.c remaps. These are the real RK3288 paths;
# only the values are ours. Every one of them must exist, because the shim remaps
# unconditionally and a missing target turns a working read into ENOENT.
#
# PRODUCT_CODE selects which device this pretends to be. This one firmware image
# serves JP07 (SC5000), JP08 (SC5000M) and JP11 (Prime GO), and /usr/Engine is shared across all of
# them, so the code is the only thing that distinguishes them.
write_fake_dt /mnt/rootfs "${PRODUCT_CODE:-JP07}"
printf '%s' 'B' > "/mnt/rootfs/root/fake-dt/inmusic,az01-pcb-rev"
# Raw big-endian <u32> devicetree cell, not text.
printf '\x00\x00\x00\x00' > "/mnt/rootfs/root/fake-dt/inmusic,internal-sd-fitted"
# /dev/mem is remapped to a plain file so a mmap of it fails cleanly rather than
# handing out real physical memory. Sparse, never read past its header.
: > /mnt/rootfs/root/fake-dev-mem
# A static /proc/interrupts, kept only as a last-resort fallback: dtshim_jc11s
# generates this content at runtime from the real /proc/interrupts, and falls back
# to this file only if that finds no usable IRQ at all.
#
# The affinity check it exists for is no longer hypothetical. Engine on RK3288 does
# perform it, and hard-throws when a name is missing:
#
#     what():  No IRQ matching 'ttyS0' found in /proc/interrupts
#
# then aborts, is restarted, and loops forever without ever reaching a display. The
# earlier version of this file listed only arch_timer and uart-pl011, so that lookup
# could never succeed and armv7 never booted to a UI.
#
# The names have to be here, but the numbers cannot be right in a static file: IRQs
# are assigned at boot from whichever devices are present, and Engine writes CPU
# affinity to /proc/irq/<N>/smp_affinity straight after finding each name. That is
# exactly why the shim generates this dynamically — this copy is a parseable floor,
# not a working configuration.
{
    printf '           CPU0       CPU1       CPU2       CPU3\n'
    printf '  1:      12345      11111      10000       9999     GIC-0  29 Level     arch_timer\n'
    printf '  2:        512          0          0          0     GIC-0  33 Level     uart-pl011\n'
    printf ' 32:       2508          0          0          0     GIC-0  74 Edge      dwc3\n'
    printf ' 33:         23          0          0          0     GIC-0  77 Edge      fe210000.sata\n'
    printf ' 34:          2          0          0          0     GIC-0  78 Edge      fea10000.dma-controller\n'
    printf ' 35:       2902          0          0          0     GIC-0  75 Edge      ff0c0000.dwmmc\n'
    printf ' 36:        107          0          0          0     GIC-0  73 Edge      ff0f0000.dwmmc\n'
    printf ' 37:        283          0          0          0     GIC-0  79 Edge      ttyS0\n'
} > /mnt/rootfs/root/fake-dt/interrupts

echo "--- wiring touchbridge.service + engine.service override ---"
# The same unit either builder installs, each from its own SoC directory: the
# binary is shared but the invocation is not -- RK3588 passes --head N and has a
# templated unit per display, RK3288 is single-head and passes a resolution.
cp -a /shims/rk3288/touchbridge/touchbridge.service /mnt/rootfs/etc/systemd/system/touchbridge.service
ln -sf ../touchbridge.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge.service

# The virtual control surface. The same unit both builders install: it names
# no device and passes no client name, because the binary works out which
# product to be from the guest's own product code.
cp -a /shims/midisurface/midisurface.service /mnt/rootfs/etc/systemd/system/midisurface.service
ln -sf ../midisurface.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/midisurface.service

echo "--- disabling the tty1 getty (Engine's display) ---"
# Engine renders fullscreen via eglfs/KMS on the same VT the console getty
# lives on, and the getty keeps reading the keyboard underneath it. Every
# keystroke therefore goes to *both* Engine and a root login shell you cannot
# see — typing into Engine's search box also types into that shell, and it is
# entirely possible to power the machine off by accident that way.
#
# Removing the enablement symlink disables it; masking getty@tty1 and
# autovt@tty1 (autovt@ is an alias of getty@, which logind spawns on VT
# allocation) stops anything bringing it back.
#
# The *serial* getty is deliberately left alone — serial-getty@ttyAMA0 is a
# different template and remains the way in on -serial stdio. Engine's own
# keyboard input is unaffected: eglfs reads evdev directly, not the VT.
rm -f /mnt/rootfs/etc/systemd/system/getty.target.wants/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/autovt@tty1.service

mkdir -p /mnt/rootfs/etc/systemd/system/engine.service.d
cat > /mnt/rootfs/etc/systemd/system/engine.service.d/override.conf <<'EOF'
[Unit]
After=touchbridge.service midisurface.service
Requires=touchbridge.service
Wants=midisurface.service

[Service]
Environment=LD_PRELOAD=/root/dtshim.so:/root/drmatomic.so:/root/alsashim.so
Environment=QT_QPA_PLATFORM=eglfs
Environment=QT_QPA_EGLFS_KMS_ATOMIC=0
# Pin the EGL device integration. Left to itself Qt logs "Using base device
# integration" — it enumerates eglfs_kms and eglfs_emu, then picks neither —
# and the base integration has no native window to give EGL, so every config
# query fails EGL_BAD_CONFIG and eglCreateWindowSurface fails EGL_BAD_NATIVE_WINDOW
# ("Could not create the egl surface: error = 0x300b", then an ABRT restart loop).
# Naming eglfs_kms outright skips whatever probe is failing here.
Environment=QT_QPA_EGLFS_INTEGRATION=eglfs_kms
# eglfs_kms is the GBM variant, so EGL has to be on the gbm platform for it;
# the vendor Mesa is built with "surfaceless" as its compiled-in default.
Environment=EGL_PLATFORM=gbm
# Software rendering, and not by preference: virgl needs virtio-gpu-gl, which the
# launchers do not attach on armhf. Not because they cannot any more — the machine
# has working PCI since it moved to highmem=off — but because virgl on a 32-bit
# guest is untested, so arch_devices.sh leaves GPU_GL_DEV empty and the GL display
# modes refuse this architecture. Revisit here too if that ever changes.
# kms_swrast is therefore the only driver that can back GBM here. Named explicitly
# rather than left to Mesa's loader, which otherwise probes for a virtio_gpu driver
# matching the kernel's device name and finds nothing usable.
Environment=MESA_LOADER_DRIVER_OVERRIDE=kms_swrast
EOF

umount /mnt/rootfs
losetup -d "$LOOPDEV"
trap - EXIT

verify_rootfs "$IMG"
DOCKER_SCRIPT

echo "--- running e2fsck/resize2fs/shim-install in a privileged container ---"
docker pull -q ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} debian:bookworm-slim >/dev/null
docker run --rm --privileged \
    ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} \
    -e OUT_NAME="$OUT_NAME" \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -e PRODUCT_CODE="${PRODUCT_CODE:-JP07}" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
    -v "$SCRIPT_DIR_SELF/rootfs_steps:/steps:ro" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:bookworm-slim bash /inner.sh

if [ ! -s "$OUT_PATH" ]; then
    echo "FAILED: expected output file is missing from $OUT_PATH." >&2
    exit 1
fi

echo ""
echo "Built: $OUT_PATH"
echo ""
file "$OUT_PATH"
echo ""
echo "Still needed to boot: kernel+initrd (get_kernel.sh --arch armhf) and a"
# Not --family engine: this rootfs's data.mount wants PARTUUID
# 931ad49d-ad59-0849-833a-9bf00af5b60e, the single az01-internal partition, which is
# the same layout the MPC images use. The disk layout tracks the platform generation,
# not the application.
echo "/data disk (make_disk.sh --family mpc) — see docs/BUILDING.md's"