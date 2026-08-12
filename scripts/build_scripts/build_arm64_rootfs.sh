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
#   6. Wire touchbridge_rmz2.service, midisurface_rmz2.service (virtual control
#      surface, auto motor-off), controllermap.service (USB controller ->
#      assignment mapping), and an engine.service.d override so engine.service
#      actually loads the shims and starts eglfs.
#   7. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind Engine's fullscreen display.
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
STAGE_DIR="$(mktemp -d /tmp/build-arm64-rootfs-stage.XXXXXX)"
trap 'rm -rf "$EXTRACT_DIR" "$STAGE_DIR"' EXIT

echo "--- building shims from source ---"
docker run --rm --platform linux/arm64 \
    -v "$SHIMS_DIR:/shims" \
    -v "$STAGE_DIR:/stage" \
    debian:bookworm bash -c '
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # libdrm-dev: drmatomic includes drm.h/drm_mode.h.
        # libasound2-dev: midisurface links libasound directly (alsashim does
        # not — it declares what it needs and resolves via dlsym).
        # libgl1-mesa-dri: staged out for the rootfs, see the /stage copy below.
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev libgl1-mesa-dri >/dev/null 2>&1

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

        # Stage the arm64 virtio_gpu/virgl-capable Mesa DRI driver for the
        # rootfs. It has to be pulled *here*, in the arm64 container, rather
        # than in the privileged container further down: that one deliberately
        # runs the host architecture (it does e2fsck/resize2fs on a multi-GB
        # image, which is far slower under qemu-user emulation), so on an
        # x86_64 host its own Mesa package is x86_64 and the wrong ABI
        # entirely. Staging it from the container that is already arm64 keeps
        # both halves correct on any host architecture.
        cp -a /usr/lib/aarch64-linux-gnu/dri/virtio_gpu_dri.so /stage/virtio_gpu_dri.so
    '

[ -s "$STAGE_DIR/virtio_gpu_dri.so" ] || {
    echo "ERROR: failed to stage an arm64 virtio_gpu_dri.so from the build container." >&2
    exit 1
}

for artifact in dtshim/dtshim_rmz2.so dtshim/drmatomic_rmz2.so \
                alsashim/alsashim_rmz2.so touchbridge_rmz2/touchbridge_rmz2 \
                midisurface_rmz2/midisurface_rmz2; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + engine.service, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-arm64-rootfs-inner.XXXXXX.sh)"
trap 'rm -rf "$EXTRACT_DIR" "$STAGE_DIR"; rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq e2fsprogs util-linux >/dev/null 2>&1
# NOTE: this container intentionally runs the *host* architecture, not arm64 —
# e2fsck/resize2fs on a multi-GB image is far slower under qemu-user emulation.
# So it must never be the source of anything that ends up inside the guest
# rootfs. The one such file, virtio_gpu_dri.so, is staged into /stage by the
# arm64 shim-build container instead.

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
# disk for QEMU's virtio-gpu to dlopen. Drop in *only* the one file —
# vendor libEGL/libgbm dlopen by filename via the standard DRI ABI, no
# libglvnd indirection to worry about, so nothing else needs to change.
# Comes from /stage, populated by the arm64 container (see the note above).
mkdir -p /mnt/rootfs/usr/lib/dri
cp -a /stage/virtio_gpu_dri.so /mnt/rootfs/usr/lib/dri/virtio_gpu_dri.so

echo "--- inserting shims into /root ---"
mkdir -p /mnt/rootfs/root/fake-dt
cp -a /shims/dtshim/dtshim_rmz2.so /mnt/rootfs/root/dtshim_rmz2.so
cp -a /shims/dtshim/drmatomic_rmz2.so /mnt/rootfs/root/drmatomic_rmz2.so
cp -a /shims/alsashim/alsashim_rmz2.so /mnt/rootfs/root/alsashim_rmz2.so
cp -a /shims/touchbridge_rmz2/touchbridge_rmz2 /mnt/rootfs/root/touchbridge_rmz2
# Started as a service (below) rather than preloaded into engine.service: it
# is a MIDI device Engine binds, not a library Engine loads.
cp -a /shims/midisurface_rmz2/midisurface_rmz2 /mnt/rootfs/root/midisurface_rmz2

echo "--- installing controllermap (USB controller -> assignment mapping) ---"
mkdir -p /mnt/rootfs/root/controllermap/mappings
cp -a /shims/controllermap/controllermap.sh /mnt/rootfs/root/controllermap/controllermap.sh
cp -a /shims/controllermap/manifest /mnt/rootfs/root/controllermap/manifest
if [ -d /shims/controllermap/mappings ]; then
    cp -a /shims/controllermap/mappings/. /mnt/rootfs/root/controllermap/mappings/
fi
chmod 755 /mnt/rootfs/root/controllermap/controllermap.sh
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

# Control surface + mapping selection. Ordering matters and is declared in the
# units themselves: controllermap picks the assignment file, then the surface
# comes up, then Engine binds it. Both must precede engine.service because
# Engine reads assignments and enumerates MIDI only during its own startup.
cp -a /shims/midisurface_rmz2/midisurface_rmz2.service /mnt/rootfs/etc/systemd/system/midisurface_rmz2.service
cp -a /shims/controllermap/controllermap.service /mnt/rootfs/etc/systemd/system/controllermap.service
ln -sf ../midisurface_rmz2.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/midisurface_rmz2.service
ln -sf ../controllermap.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/controllermap.service

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
# It also deepens the PCM ring, which is what makes playback clean rather than
# merely present: Engine asks for 256 frames (5.8ms) at 44100Hz, shorter than
# the 10ms timer QEMU's audio subsystem services the emulated card on, so the
# card reads ring content Engine hasn't refilled. ALSASHIM_BUFFER_SCALE
# multiplies the ring depth only (the 128-frame period, i.e. Engine's callback
# rate, is untouched); the built-in default is 8 -> 2048 frames / 46ms. Raise
# it if playback still glitches, lower it to trade headroom back for latency,
# or set 1 to leave Engine's own buffering alone entirely.
#Environment=ALSASHIM_BUFFER_SCALE=8
EOF

# Engine must start after the control surface exists, or it will never bind it
# (its MIDI enumerator only scans during startup).
cat > /mnt/rootfs/etc/systemd/system/engine.service.d/midisurface.conf <<'EOF'
[Unit]
After=midisurface_rmz2.service controllermap.service
Wants=midisurface_rmz2.service
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
    ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} \
    -e OUT_NAME="$OUT_NAME" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
    -v "$STAGE_DIR:/stage:ro" \
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