#!/bin/bash
# Automates extraction and modification of a stock *arm64 / RK3588* Engine OS rootfs
# for QEngine.
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image with binwalk 3.
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build alsashim (the only shim built here rather than committed
#      prebuilt — see the build step below).
#   5. Copy the dtshim/drmatomic/alsashim/teeshim/touchbridge shims +
#      fake-dt files into /root.
#   6. Wire touchbridge.service, midisurface.service (virtual control
#      surface), controllermap.service (USB controller ->
#      assignment mapping), and an engine.service.d override so engine.service
#      actually loads the shims and starts eglfs.
#   7. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind the fullscreen display.
#   8. Copy in a real virtio_gpu/virgl-capable Mesa DRI drive
#
# Usage: build_arm64_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  *-Update.img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Environment:
#   PRODUCT_CODE  which of this image's device identities to spoof. Default RMZ2.
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
SHIM_ARCH="arm64"

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
STAGE_DIR="$(mktemp -d /tmp/build-arm64-rootfs-stage.XXXXXX)"
trap 'rm -rf "$STAGE_DIR"' EXIT

echo "--- building shims from source ---"
# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform linux/arm64 debian:bookworm >/dev/null
docker run --rm --platform linux/arm64 \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -v "$SHIMS_DIR:/shims" \
    -v "$STAGE_DIR:/stage" \
    debian:bookworm bash -c '
        set -e
        # These shims and the staged DRI driver are copied straight into an arm64 rootfs, so a
        # wrong-architecture container here would graft foreign binaries in. Fail loudly instead.
        case "$(uname -m)" in aarch64|arm64) ;; *)
            echo "ERROR: shim container is $(uname -m), expected aarch64." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # libdrm-dev: drmatomic includes drm.h/drm_mode.h.
        # libasound2-dev: midisurface links libasound directly (alsashim does
        # not — it declares what it needs and resolves via dlsym).
        # libgl1-mesa-dri: staged out for the rootfs, see the /stage copy below.
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev libgl1-mesa-dri >/dev/null 2>&1

        gcc -shared -fPIC -O2 -Wall \
            -o /shims/dtshim/dtshim_$SHIM_ARCH.so /shims/dtshim/dtshim.c -DSOC_RK3588 -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /shims/drmatomic/drmatomic_$SHIM_ARCH.so /shims/drmatomic/drmatomic.c -ldl
        gcc -O2 -Wall \
            -o /shims/touchbridge/touchbridge_$SHIM_ARCH /shims/touchbridge/touchbridge.c
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/alsashim/alsashim_$SHIM_ARCH.so /shims/alsashim/alsashim.c -ldl
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/teeshim/teeshim_$SHIM_ARCH.so /shims/teeshim/teeshim.c
        gcc -O2 -Wall \
            -o /shims/midisurface/midisurface_$SHIM_ARCH /shims/midisurface/midisurface.c -lasound

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

# The shared shims are checked separately: they live outside SHIMS_DIR, and each
# one is named for the architecture it was built for rather than for a device.
for artifact in alsashim/alsashim_$SHIM_ARCH.so drmatomic/drmatomic_$SHIM_ARCH.so \
                touchbridge/touchbridge_$SHIM_ARCH midisurface/midisurface_$SHIM_ARCH \
                teeshim/teeshim_$SHIM_ARCH.so dtshim/dtshim_$SHIM_ARCH.so; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + engine.service, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-arm64-rootfs-inner.XXXXXX.sh)"
trap 'rm -rf "$STAGE_DIR"; rm -f "$INNER_SCRIPT"' EXIT

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
if [ ! -e /mnt/rootfs/lib/ld-linux-aarch64.so.1 ]; then
    echo "ERROR: this firmware's rootfs is not arm64 (no /lib/ld-linux-aarch64.so.1)." >&2
    echo "       This builds arm64/RK3588 Engine OS firmware only." >&2
    exit 1
fi
if [ ! -d /mnt/rootfs/usr/Engine ]; then
    echo "ERROR: no /usr/Engine in this rootfs, so it is not an Engine OS image." >&2
    exit 1
fi

block_telemetry /mnt/rootfs
blank_root_password /mnt/rootfs
skip_firmware_update /mnt/rootfs

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
cp -a /shims/dtshim/dtshim_$SHIM_ARCH.so /mnt/rootfs/root/dtshim.so
cp -a /shims/drmatomic/drmatomic_$SHIM_ARCH.so /mnt/rootfs/root/drmatomic.so
cp -a /shims/touchbridge/touchbridge_$SHIM_ARCH /mnt/rootfs/root/touchbridge
cp -a /shims/alsashim/alsashim_$SHIM_ARCH.so /mnt/rootfs/root/alsashim.so
# Started as a service (below) rather than preloaded into engine.service: it
# is a MIDI device Engine binds, not a library Engine loads.
cp -a /shims/midisurface/midisurface_$SHIM_ARCH /mnt/rootfs/root/midisurface
chmod 755 /mnt/rootfs/root/dtshim.so \
          /mnt/rootfs/root/drmatomic.so \
          /mnt/rootfs/root/touchbridge \
          /mnt/rootfs/root/alsashim.so \
          /mnt/rootfs/root/midisurface

# PRODUCT_CODE selects which device this pretends to be.
write_fake_dt /mnt/rootfs "${PRODUCT_CODE:-RMZ2}"

# Only this build installs teeshim: it bypasses a TEE check that only Engine
# 5.1.0+ on RK3588 makes, and there is no armv7 counterpart.
cp -a /shims/teeshim/teeshim_$SHIM_ARCH.so /mnt/rootfs/root/teeshim.so
chmod 755 /mnt/rootfs/root/teeshim.so

echo "--- installing controllermap (USB controller -> assignment mapping) ---"
mkdir -p /mnt/rootfs/root/controllermap/mappings
cp -a /shims/rk3588/controllermap/controllermap.sh /mnt/rootfs/root/controllermap/controllermap.sh
cp -a /shims/rk3588/controllermap/manifest /mnt/rootfs/root/controllermap/manifest
if [ -d /shims/rk3588/controllermap/mappings ]; then
    cp -a /shims/rk3588/controllermap/mappings/. /mnt/rootfs/root/controllermap/mappings/
fi
chmod 755 /mnt/rootfs/root/controllermap/controllermap.sh

echo "--- wiring touchbridge.service + engine.service override ---"
# The same unit either builder installs, each from its own SoC directory: the
# binary is shared but the invocation is not -- RK3588 passes --head N and has a
# templated unit per display, RK3288 is single-head and passes a resolution.
cp -a /shims/rk3588/touchbridge/touchbridge.service /mnt/rootfs/etc/systemd/system/touchbridge.service
ln -sf ../touchbridge.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge.service

# Per-head touch, for the multi-display product.
#
# One bridge per head: QEMU gives each head its own usb-tablet (GPU_MAX_OUTPUTS
# on the launcher), and each instance turns its own tablet into a touchscreen and
# publishes /dev/input/qengine-touchN for that output's touchDevice to resolve.
# Head 0 is the non-templated unit above; 1 and 2 are instantiated here.
#
# Enabled unconditionally because the head count is a runtime property the build
# cannot see. An instance whose head does not exist waits for its tablet, reports
# that this guest has fewer displays, and exits 0 — so on a single-screen guest
# they settle inactive instead of restart-looping.
cp -a /shims/rk3588/touchbridge/touchbridge@.service /mnt/rootfs/etc/systemd/system/touchbridge@.service
ln -sf ../touchbridge@.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge@1.service
ln -sf ../touchbridge@.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge@2.service

echo "--- pointing JP22's screen configuration at what the guest actually has ---"
# Only the output *names* and the touch devices are wrong for emulation; the
# three-display layout itself is real and now genuinely served (GPU_MAX_OUTPUTS=3).
#
#   name        stock eDP1/DSI2/DSI1 match no connector here, and Qt falls back to
#               defaults for an unmatched output — which is why touch went nowhere
#               and why the stock file could not bind anything. Qt names virtio-gpu
#               connectors Virtual1..VirtualN (its own naming, note: no dash, unlike
#               sysfs's card0-Virtual-1).
#   mode        QEMU applies xres/yres to scanout 0 only, leaving heads 1 and 2 at
#               an 800x600 default; asking for the mode explicitly squares them up.
#   touchDevice the stock platform-*.i2c-event paths do not exist. Engine resolves
#               each of these as a symlink and substitutes the target, so they must
#               be symlinks — which is what touchbridge publishes.
#
# rotation is left exactly as shipped: dtshim already serves all three of those
# devicetree paths.
JP22_SCREEN_CFG=/mnt/rootfs/usr/Engine/ScreenConfiguration/JP22/ScreenConfiguration.json
if [ -f "$JP22_SCREEN_CFG" ]; then
    [ -f "$JP22_SCREEN_CFG.stock" ] || cp -a "$JP22_SCREEN_CFG" "$JP22_SCREEN_CFG.stock"
    cat > "$JP22_SCREEN_CFG" <<'EOF'
{
    "hwcursor": false,
    "outputs": [
        {
            "name": "Virtual1",
            "mode": "1280x800",
            "touchDevice": "/dev/input/qengine-touch0",
            "rotation": "/sys/firmware/devicetree/base/edp-panel/rotation",
            "virtualIndex": 0
        },
        {
            "name": "Virtual2",
            "mode": "1280x800",
            "touchDevice": "/dev/input/qengine-touch1",
            "rotation": "/sys/firmware/devicetree/base/dsi@fde30000/panel@0/rotation",
            "primary": true,
            "virtualIndex": 1
        },
        {
            "name": "Virtual3",
            "mode": "1280x800",
            "touchDevice": "/dev/input/qengine-touch2",
            "rotation": "/sys/firmware/devicetree/base/dsi@fde20000/panel@0/rotation",
            "virtualIndex": 2
        }
    ]
}
EOF
else
    echo "    (this firmware ships no JP22 screen configuration — skipping)"
fi

# Control surface + mapping selection. Ordering matters and is declared in the
# units themselves: controllermap picks the assignment file, then the surface
# comes up, then Engine binds it. Both must precede engine.service because
# Engine reads assignments and enumerates MIDI only during its own startup.
cp -a /shims/midisurface/midisurface.service /mnt/rootfs/etc/systemd/system/midisurface.service
cp -a /shims/rk3588/controllermap/controllermap.service /mnt/rootfs/etc/systemd/system/controllermap.service
ln -sf ../midisurface.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/midisurface.service
ln -sf ../controllermap.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/controllermap.service

echo "--- disabling the tty1 getty (Engine's display) ---"
# Engine renders fullscreen via eglfs/KMS on the same VT the console getty
# lives on, and the getty keeps reading the keyboard underneath it. Every
# keystroke therefore goes to *both* Engine and a root login shell you cannot
# see
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
After=touchbridge.service
Requires=touchbridge.service

[Service]
Environment=LD_PRELOAD=/root/dtshim.so:/root/drmatomic.so:/root/alsashim.so:/root/teeshim.so
Environment=QT_QPA_PLATFORM=eglfs
Environment=QT_QPA_EGLFS_KMS_ATOMIC=0
# teeshim: answers the OP-TEE attestation Engine 5.1.0 runs before starting its
# GUI. QEMU has no TrustZone secure world, so the real call fails and Engine
# quits into /usr/bin/test-app-launcher instead of ever showing Engine. Harmless
# on 5.0.4, which makes no TEE calls at all. See the shim source for the full
# check and TEESHIM_DEBUG for per-call logging.
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
After=midisurface.service controllermap.service
Wants=midisurface.service
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
    -e PRODUCT_CODE="${PRODUCT_CODE:-RMZ2}" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
    -v "$SCRIPT_DIR_SELF/rootfs_steps:/steps:ro" \
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
echo "Still needed to boot: kernel+initrd (get_kernel.sh --arch arm64) and a"
echo "/data+/factory disk (make_disk.sh --family engine) — see docs/BUILDING.md's"