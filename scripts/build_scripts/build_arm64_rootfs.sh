#!/bin/bash
# Automates extraction and modifcation of stock Engine rootfs for QEngine
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image with binwalk 3.
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block Sentry telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build alsashim_rmz2.so (the only shim built here rather than committed
#      prebuilt — see the build step below).
#   5. Copy the dtshim/drmatomic/alsashim/touchbridge_rmz2 shims + fake-dt files into /root.
#   6. Wire touchbridge_rmz2.service + an engine.service.d override so engine.service actually loads the shims and starts eglfs.
#   7. Blank the root password for passwordless serial-console login.
#   8. Copy in a real virtio_gpu/virgl-capable Mesa DRI drive
#
# Usage: build_arm64_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  *-Update.img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Requires: binwalk (3.x), qemu-img, docker.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SHIMS_DIR="$REPO_ROOT/shims/rk3588"

OUT_PATH="$REPO_ROOT/build/rootfs_out.img"
SIZE=4294967296
FORCE=0

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

for bin in binwalk qemu-img docker; do
    command -v "$bin" >/dev/null 2>&1 || { echo "ERROR: '$bin' is required but not found on PATH." >&2; exit 1; }
done

OUT_DIR="$(cd "$(dirname "$OUT_PATH")" && pwd)"
OUT_NAME="$(basename "$OUT_PATH")"
mkdir -p "$OUT_DIR"

### 1. Extract the rootfs partition with binwalk ############################

EXTRACT_DIR="$(mktemp -d /tmp/build-arm64-rootfs-extract.XXXXXX)"
trap 'rm -rf "$EXTRACT_DIR"' EXIT

echo "--- extracting $FIRMWARE_IMG with binwalk (this scans the whole image, ~10s+) ---"
binwalk -e -C "$EXTRACT_DIR" "$FIRMWARE_IMG"

# binwalk 3 signature-scans rather than parsing the firmware container
# format, so it finds every embedded ext2/3/4 filesystem — the real rootfs
# (~830MB) plus two much smaller redundant boot-slot partitions. Identify
# the rootfs by picking the largest ext2/3/4 image found, rather than
# hardcoding an offset that's specific to this one firmware build.
BEST_CANDIDATE=""
BEST_SIZE=0
while IFS= read -r -d '' f; do
    if file "$f" | grep -q 'ext[234] filesystem'; then
        f_size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
        if [ "$f_size" -gt "$BEST_SIZE" ]; then
            BEST_CANDIDATE="$f"
            BEST_SIZE="$f_size"
        fi
    fi
done < <(find "$EXTRACT_DIR" -type f -print0)

if [ -z "$BEST_CANDIDATE" ]; then
    echo "ERROR: no ext2/3/4 filesystem image found in binwalk's extraction output." >&2
    exit 1
fi

echo "--- found rootfs candidate: $BEST_CANDIDATE ($((BEST_SIZE / 1024 / 1024)) MiB) ---"
cp "$BEST_CANDIDATE" "$OUT_PATH"

### 2. Grow the image and filesystem #########################################

echo "--- resizing image to $SIZE bytes ---"
qemu-img resize -f raw "$OUT_PATH" "$SIZE"

### 2b. Build the shims ######################################################
# The shim binaries are .gitignored (*.so, plus touchbridge_rmz2 by name), so
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
docker run --rm --platform linux/arm64 \
    -v "$SHIMS_DIR:/shims" \
    debian:bookworm bash -c '
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # libdrm-dev: drmatomic includes drm.h/drm_mode.h.
        # libasound2-dev: midisurface links libasound directly (alsashim does
        # not — it declares what it needs and resolves via dlsym).
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1

        gcc -shared -fPIC -O2 -Wall \
            -o /shims/dtshim/dtshim_rmz2.so /shims/dtshim/dtshim_rmz2.c -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /shims/dtshim/drmatomic_rmz2.so /shims/dtshim/drmatomic_rmz2.c -ldl
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/alsashim/alsashim_rmz2.so /shims/alsashim/alsashim_rmz2.c -ldl
        gcc -O2 -Wall \
            -o /shims/touchbridge_rmz2/touchbridge_rmz2 /shims/touchbridge_rmz2/touchbridge_rmz2.c
        gcc -O2 -Wall \
            -o /shims/midisurface_rmz2/midisurface_rmz2 /shims/midisurface_rmz2/midisurface_rmz2.c -lasound
    '

for artifact in dtshim/dtshim_rmz2.so dtshim/drmatomic_rmz2.so \
                alsashim/alsashim_rmz2.so touchbridge_rmz2/touchbridge_rmz2 \
                midisurface_rmz2/midisurface_rmz2; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + engine.service, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-arm64-rootfs-inner.XXXXXX.sh)"
trap 'rm -rf "$EXTRACT_DIR"; rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq e2fsprogs util-linux >/dev/null 2>&1
# libgl1-mesa-dri: source of a real virtio_gpu/virgl-capable DRI driver
# (see the /usr/lib/dri step below) — this container's own arch matches
# the target (arm64), so a plain install pulls the right package without
# needing the ar/tar .deb-extraction dance docs/BUILDING.md describes for
# extracting one on macOS directly.
apt-get install -y -qq libgl1-mesa-dri >/dev/null 2>&1

IMG="/out/$OUT_NAME"

echo "--- e2fsck (required before resize2fs) ---"
set +e
e2fsck -f -y "$IMG"
FSCK_RC=$?
set -e
# 0 = clean, 1/2 = errors found and corrected — all fine to proceed from.
# Anything higher means e2fsck couldn't fix it.
if [ "$FSCK_RC" -gt 2 ]; then
    echo "ERROR: e2fsck failed with exit code $FSCK_RC" >&2
    exit 1
fi

echo "--- resize2fs ---"
resize2fs "$IMG"

echo "--- mounting via loop device ---"
LOOPDEV="$(losetup -f)"
losetup "$LOOPDEV" "$IMG"
mkdir -p /mnt/rootfs
# extents/64bit are ext4 features even though `file` labels this ext2
# (no journal) — mount as ext4 so the kernel driver understands them.
mount -t ext4 "$LOOPDEV" /mnt/rootfs
cleanup() { umount /mnt/rootfs || true; losetup -d "$LOOPDEV" || true; }
trap cleanup EXIT

echo "--- blocking Sentry telemetry (docs/BLOCKING_TELEMETRY.md) ---"
TELEMETRY_LINE="127.0.0.1 o230257.ingest.sentry.io"
grep -qxF "$TELEMETRY_LINE" /mnt/rootfs/etc/hosts || echo "$TELEMETRY_LINE" >> /mnt/rootfs/etc/hosts

echo "--- blanking root password for serial-console login ---"
sed -i 's|^root:[^:]*:|root::|' /mnt/rootfs/etc/shadow

echo "--- installing a virtio_gpu/virgl-capable Mesa DRI driver ---"
# This rootfs has no /usr/lib/dri at all — real hardware only ever needed
# Panthor (kernel-side, panthor.ko), so there's no userspace DRI driver on
# disk for QEMU's virtio-gpu to dlopen. Pull one from Debian bookworm's own
# distro-packaged Mesa (installed above) and drop in *only* the one file —
# vendor libEGL/libgbm dlopen by filename via the standard DRI ABI, no
# libglvnd indirection to worry about, so nothing else needs to change.
mkdir -p /mnt/rootfs/usr/lib/dri
cp -a /usr/lib/aarch64-linux-gnu/dri/virtio_gpu_dri.so /mnt/rootfs/usr/lib/dri/virtio_gpu_dri.so

echo "--- inserting shims into /root ---"
mkdir -p /mnt/rootfs/root/fake-dt
cp -a /shims/dtshim/dtshim_rmz2.so /mnt/rootfs/root/dtshim_rmz2.so
cp -a /shims/dtshim/drmatomic_rmz2.so /mnt/rootfs/root/drmatomic_rmz2.so
cp -a /shims/alsashim/alsashim_rmz2.so /mnt/rootfs/root/alsashim_rmz2.so
cp -a /shims/touchbridge_rmz2/touchbridge_rmz2 /mnt/rootfs/root/touchbridge_rmz2
# Not preloaded into engine.service — a manually-run tool for driving the
# emulated unit's transport, which has no on-screen equivalent. See
# docs/BUILDING.md.
cp -a /shims/midisurface_rmz2/midisurface_rmz2 /mnt/rootfs/root/midisurface_rmz2
cp -a "/shims/dtshim/fake-dt-rmz2/inmusic,product-code" /mnt/rootfs/root/fake-dt/
cp -a /shims/dtshim/fake-dt-rmz2/serial-number /mnt/rootfs/root/fake-dt/
cp -a /shims/dtshim/fake-dt-rmz2/interrupts /mnt/rootfs/root/fake-dt/
# Raw big-endian <u32> devicetree cell, not text — 0 (no rotation), the
# value confirmed working against RMZ2's real panel orientation.
printf '\x00\x00\x00\x00' > /mnt/rootfs/root/fake-dt/rotation
chmod 755 /mnt/rootfs/root/dtshim_rmz2.so /mnt/rootfs/root/drmatomic_rmz2.so \
          /mnt/rootfs/root/alsashim_rmz2.so /mnt/rootfs/root/touchbridge_rmz2 \
          /mnt/rootfs/root/midisurface_rmz2

echo "--- wiring touchbridge_rmz2.service + engine.service override ---"
cp -a /shims/touchbridge_rmz2/touchbridge_rmz2.service /mnt/rootfs/etc/systemd/system/touchbridge_rmz2.service
ln -sf ../touchbridge_rmz2.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge_rmz2.service

mkdir -p /mnt/rootfs/etc/systemd/system/engine.service.d
cat > /mnt/rootfs/etc/systemd/system/engine.service.d/override.conf <<'EOF'
[Service]
After=touchbridge_rmz2.service
Requires=touchbridge_rmz2.service
Environment=LD_PRELOAD=/root/dtshim_rmz2.so:/root/drmatomic_rmz2.so:/root/alsashim_rmz2.so
Environment=QT_QPA_PLATFORM=eglfs
Environment=QT_QPA_EGLFS_KMS_ATOMIC=0
# alsashim: gets an emulated sound card past Engine's compiled-in card-name
# allowlist and routes its PCM opens through ALSA's format-converting plug
# layer. Card 0 is QEMU's emulated HDA controller; attach it playback-only
# (-device hda-output, not hda-duplex) or Engine picks the capture device as
# its default and never drives playback at all. See docs/ENGINEOS.md.
Environment=ALSASHIM_CARD=0
EOF

umount /mnt/rootfs
losetup -d "$LOOPDEV"
trap - EXIT

echo "--- final consistency check ---"
e2fsck -f -y "$IMG" || true

if [ ! -s "$IMG" ]; then
    echo "ERROR: output image missing or empty — something failed silently above." >&2
    exit 1
fi
DOCKER_SCRIPT

echo "--- running e2fsck/resize2fs/shim-install in a privileged container ---"
docker run --rm --privileged \
    -e OUT_NAME="$OUT_NAME" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
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
echo "Still needed to boot: kernel+initrd (get_arm64_kernel.sh) and a"
echo "/data+/factory disk (make_data_disk.sh) — see docs/BUILDING.md's"