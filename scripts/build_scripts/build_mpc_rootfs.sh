#!/bin/bash
# Automates extraction and modification of a stock Akai MPC rootfs for QEngine.
# The armv7/RK3288 sibling of build_arm64_rootfs.sh; MPC needs far less doing to
# it than Engine does, because it drives KMS directly and links no Mali/EGL.
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image with binwalk 3.
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build touchbridge and dtshim for armhf (the shared sources under shims/
#      are architecture-independent — see the build step below).
#   5. Copy them into /root, wire touchbridge_mpc.service ahead of acvs.service,
#      and preload dtshim into acvs.service so MPC can read a product code.
#   6. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind the fullscreen display.
#
# Usage: build_mpc_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  Firmware .img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Environment:
#   PRODUCT_CODE  which of this image's device identities to spoof. Default ACV5.
#
# Requires: binwalk (3.x), qemu-img, docker.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR_SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIMS_DIR="$REPO_ROOT/shims"
# The builders mount the whole shims tree, so a shim shared between them is
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
# debian:bookworm for glibc 2.36, comfortably older than the guest's
# — see docs/BUILDING.md's "Toolchain for
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
        set -e
        # These shims are copied straight into an armv7 rootfs, so a
        # wrong-architecture container here would graft foreign binaries in. Fail loudly instead.
        case "$(uname -m)" in armv7l|armv8l|armhf) ;; *)
            echo "ERROR: shim container is $(uname -m), expected armv7l." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc libc6-dev >/dev/null 2>&1

        gcc -shared -fPIC -O2 -Wall \
            -o /shims/dtshim/dtshim_$SHIM_ARCH.so /shims/dtshim/dtshim.c -DSOC_RK3288 -ldl -lpthread
        gcc -O2 -Wall \
            -o /shims/touchbridge/touchbridge_$SHIM_ARCH /shims/touchbridge/touchbridge.c
    '

# The shared shims are checked separately: they live outside SHIMS_DIR, and each
# one is named for the architecture it was built for rather than for a device.
for artifact in touchbridge/touchbridge_$SHIM_ARCH \
                dtshim/dtshim_$SHIM_ARCH.so; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + root password, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-mpc-rootfs-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq e2fsprogs util-linux >/dev/null 2>&1
# NOTE: this container intentionally runs the *host* architecture, not armhf —
# e2fsck/resize2fs on a multi-GB image is far slower under qemu-user emulation.
# So it must never be the source of anything that ends up inside the guest
# rootfs. Nothing here is: the shims come from the armhf container above.

IMG="/out/$OUT_NAME"

# The steps the rootfs builders share, so a change to one lands in all of them.
# Each file in rootfs_steps/ defines one function and explains what it is for. The
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
    echo "       This builds armv7/RK3288 MPC firmware only." >&2
    echo "       The Gen 2 MPC image is arm64 and will not boot the armhf kernel." >&2
    exit 1
fi
if [ ! -d /mnt/rootfs/usr/bin/MPC ]; then
    echo "ERROR: no /usr/bin/MPC in this rootfs, so it is not an MPC image." >&2
    exit 1
fi

block_telemetry /mnt/rootfs
blank_root_password /mnt/rootfs

echo "--- inserting shims into /root ---"
cp -a /shims/dtshim/dtshim_$SHIM_ARCH.so /mnt/rootfs/root/dtshim.so
chmod 755 /mnt/rootfs/root/dtshim.so

# The devicetree properties dtshim.c remaps. These are the real RK3288 paths;
# only the values are ours. Every one of them must exist, because the shim remaps
# unconditionally and a missing target turns a working read into ENOENT.
#
# PRODUCT_CODE selects which device this pretends to be.
write_fake_dt /mnt/rootfs "${PRODUCT_CODE:-ACV5}"

echo "--- wiring touchbridge.service ---"
# QEMU's usb-kbd/usb-tablet are unreachable on the 32-bit virt machine (no PCI,
# so no USB controller), and the virtio tablet that replaces them presents as an
# absolute *mouse*. MPC only responds to a real touchscreen, so the bridge
# re-emits that pointer as a uinput multitouch device. It must start before
# acvs.service: MPC enumerates input once, at its own startup.
cp -a /shims/touchbridge/touchbridge_$SHIM_ARCH /mnt/rootfs/root/touchbridge_mpc
chmod 755 /mnt/rootfs/root/touchbridge_mpc
cp -a /shims/rk3288/touchbridge_mpc/touchbridge_mpc.service /mnt/rootfs/etc/systemd/system/touchbridge_mpc.service
ln -sf ../touchbridge_mpc.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge_mpc.service

echo "--- disabling the tty1 getty (Application's display) ---"
# MPC renders fullscreen via KMS on the same VT the console getty
# lives on, and the getty keeps reading the keyboard underneath it. Every
# keystroke therefore goes to *both* the app and a root login shell you cannot
# see.
#
# Removing the enablement symlink disables it; masking getty@tty1 and
# autovt@tty1 (autovt@ is an alias of getty@, which logind spawns on VT
# allocation) stops anything bringing it back.
#
# The *serial* getty is deliberately left alone — serial-getty@ttyAMA0 is a
# different template and remains the way in on -serial stdio.
rm -f /mnt/rootfs/etc/systemd/system/getty.target.wants/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/autovt@tty1.service


echo "--- preloading dtshim into acvs.service ---"
# A drop-in rather than an edit, so the vendor unit stays as shipped. systemd
# reads drop-ins after the main unit and last assignment wins for a given
# variable, so this would silently replace an LD_PRELOAD the vendor unit set.
# Nothing in this firmware sets one; the guard keeps that from becoming a silent
# breakage if a future firmware does.
for _unit in /mnt/rootfs/etc/systemd/system/acvs.service \
             /mnt/rootfs/lib/systemd/system/acvs.service \
             /mnt/rootfs/usr/lib/systemd/system/acvs.service; do
    [ -f "$_unit" ] || continue
    if grep -q 'LD_PRELOAD' "$_unit"; then
        echo "ERROR: $_unit already sets LD_PRELOAD; this drop-in would replace it." >&2
        echo "       Merge the two by hand rather than overriding blindly." >&2
        exit 1
    fi
done
mkdir -p /mnt/rootfs/etc/systemd/system/acvs.service.d
cat > /mnt/rootfs/etc/systemd/system/acvs.service.d/override.conf <<'EOF'
[Service]
Environment=LD_PRELOAD=/root/dtshim.so
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
    -e PRODUCT_CODE="${PRODUCT_CODE:-ACV5}" \
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
echo "Still needed to boot: kernel+initrd (get_kernel.sh --arch armhf) and an"
echo "/data disk (make_disk.sh --family mpc) — see BUILD_MPC.md."