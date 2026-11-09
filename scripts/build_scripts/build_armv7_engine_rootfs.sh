#!/bin/bash
# Automates extraction and modification of a stock *armv7 / RK3288* Engine OS rootfs
# for QEngine.
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image (extract_rootfs.sh).
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build the dtshim/drmatomic/touchbridge/alsashim/quitshim shims for armhf.
#      Only dtshim is RK3288-specific; the rest compile from the shared sources
#      unmodified.
#   5. Copy those shims + fake-dt files into /root.
#   6. Wire touchbridge.service, midisurface.service (virtual control
#      surface) and an engine.service.d override so
#      engine.service actually loads the shims and starts eglfs.
#   7. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind the fullscreen display.
#
# Nothing is staged into /usr/lib — this rootfs already ships everything the
# graphics stack needs, and step 6's environment is what points Qt and Mesa at it.
# See the note above the shim install for the two things that were staged while
# that was still being worked out, and why neither is needed.
#
# Not carried over from the arm64 build: controllermap, which exists to swap a
# real USB controller's assignment files in and hardcodes RMZ2's directory.
#
# Usage: build_armv7_engine_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  Firmware .img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Environment:
#   PRODUCT_CODE  which of this image's device identities to spoof. Default JP07,
#                 or HG06 for a HeadRush image.
#   MONTERO       1 to build MONTERO, the QML mod loader, with every mod under
#                 shims/montero/mods, install them in /root, and preload MONTERO
#                 into Engine. Off by default: it is the one shim that needs Qt to
#                 build, which costs this build its own container.
#   BINWALK       1 to extract with binwalk instead of QEngine's readers. See
#                 extract_rootfs.sh.
#
# Requires: python3 (binwalk with BINWALK=1), qemu-img, docker.
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

EXTRACTOR=python3; [ "${BINWALK:-0}" = 1 ] && EXTRACTOR=binwalk
for bin in "$EXTRACTOR" qemu-img docker file; do
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

### 1. Extract the rootfs partition #########################################

# Shared with the other rootfs builder and with new_instance.sh, which needs the
# same extraction to identify a firmware's device family before it can choose
# between us. It sets EXTRACTED_ROOTFS_SIZE and cleans up its own scratch dir.
# shellcheck source=extract_rootfs.sh
. "$SCRIPT_DIR_SELF/extract_rootfs.sh"
# Which Mesa this firmware ships, and in which layout. Read off the rootfs rather
# than assumed from the architecture: armv7 had no Mesa at all before 5.0.0, where
# GL came from a proprietary Mali blob, and 5.0.x ships 24.0.7 in the DRI layout
# while the same firmware version on arm64 ships 24.3.4 in the gallium one. See
# detect_mesa.sh.
# shellcheck source=detect_mesa.sh
. "$SCRIPT_DIR_SELF/detect_mesa.sh"

extract_rootfs "$FIRMWARE_IMG" "$OUT_PATH"

# The early SC5000 application is named JP07, before /usr/Engine/Engine.
# Keep it in this builder, but select its guest-ABI/VNC stack before building
# any of the modern preloads or trying the Qt 5.15 Mali replacement.
DEBUGFS="$(command -v debugfs || echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs)"
LEGACY_ENGINE=0
if "$DEBUGFS" -R 'stat /usr/JP07/JP07' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:' ||
        "$DEBUGFS" -R 'stat /usr/Engine/JP07' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:'; then
    LEGACY_ENGINE=1
fi
if [ "$LEGACY_ENGINE" = 1 ]; then
    MESA_LAYOUT=none
    MESA_VERSION=""
    echo "--- legacy JP07 runtime: software VNC, vendor GL retained ---"
else
    detect_mesa "$OUT_PATH"
fi

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
STAGE_DIR="$(mktemp -d /tmp/build-armv7-engine-rootfs-stage.XXXXXX)"
trap 'rm -rf "$STAGE_DIR"' EXIT

if [ "$LEGACY_ENGINE" = 1 ]; then
    [ "${MONTERO:-0}" != 1 ] || {
        echo "ERROR: MONTERO requires Qt 6; this JP07 firmware uses Qt 5." >&2; exit 1; }
    echo "--- building legacy shims against the guest runtime ---"
    "$SCRIPT_DIR_SELF/build_legacy_engine_shims.sh" "$OUT_PATH" "$STAGE_DIR"
    MONTERO=0
else
echo "--- building shims from source ---"
# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform linux/arm/v7 debian:bookworm >/dev/null
docker run --rm --platform linux/arm/v7 \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -v "$SHIMS_DIR:/shims" \
    -v "$STAGE_DIR:/stage" \
    debian:bookworm bash -c '
        set -e
        # These shims are copied straight into an armv7 rootfs, so a
        # wrong-architecture container here would graft foreign binaries in. Fail loudly instead.
        case "$(uname -m)" in armv7l|armv8l|armhf) ;; *)
            echo "ERROR: shim container is $(uname -m), expected armv7l." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # libdrm-dev: drmatomic includes drm.h/drm_mode.h. No libgl1-mesa-dri:
        # unlike the RMZ2 rootfs this one already ships a complete Mesa, so
        # nothing foreign needs staging in. See the privileged container below.
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1

        gcc -shared -fPIC -O2 -Wall \
            -o /shims/dtshim/dtshim_$SHIM_ARCH.so /shims/dtshim/dtshim.c -DSOC_RK3288 -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /shims/drmatomic/drmatomic_$SHIM_ARCH.so /shims/drmatomic/drmatomic.c -ldl
        gcc -O2 -Wall \
            -o /shims/touchbridge/touchbridge_$SHIM_ARCH /shims/touchbridge/touchbridge.c
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/alsashim/alsashim_$SHIM_ARCH.so /shims/alsashim/alsashim.c -ldl
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/quitshim/quitshim_$SHIM_ARCH.so /shims/quitshim/quitshim.c -ldl -lpthread
        gcc -O2 -Wall \
            -o /shims/midisurface/midisurface_$SHIM_ARCH /shims/midisurface/midisurface.c -lasound
        gcc -O2 -Wall \
            -o /shims/pcmhold/pcmhold_$SHIM_ARCH /shims/pcmhold/pcmhold.c -lasound
        # -lpthread for its upstream relay thread. Only this builder compiles it:
        # it proxies the Engine Qt VNC server, and Qt dropped the VNC platform
        # plugin, so there is nothing for it to proxy on arm64 or on armv7 Engine
        # from 5.0.4 on. See docs/BUILDING.md section 8.
        gcc -O2 -Wall \
            -o /shims/rk3288/vnctouchbridge/vnctouchbridge_$SHIM_ARCH \
               /shims/rk3288/vnctouchbridge/vnctouchbridge.c -lpthread
    '

# MONTERO, the QML mod loader, only when asked for: see build_montero.sh.
MONTERO="${MONTERO:-0}"
if [ "$MONTERO" = 1 ]; then
    "$SCRIPT_DIR_SELF/build_montero.sh" --arch "$SHIM_ARCH" --stage "$STAGE_DIR"
fi

# The virgl driver, built to match what detect_mesa found and cached in build/ the
# way get_kernel.sh caches a kernel. Not taken from Debian's libgl1-mesa-dri: that
# package's driver is one megadriver holding every gallium driver, so it needs
# libLLVM and the AMD/nouveau libdrms, none of which this rootfs has -- and Mesa
# responds to the failed dlopen by falling back to swrast in total silence. Nor at a
# version of our choosing: a driver from another release is an ABI gamble against
# the loader the guest ships. See build_virgl_mesa.sh, and install_virgl_mesa.sh for
# what each layout does to the rootfs.
if [ "$MESA_LAYOUT" = none ]; then
    echo "--- no Mesa in this firmware, so there is no virgl driver to build ---"
elif [ "$MESA_LAYOUT" = mali ]; then
    # 4.3.4 and earlier: a whole Mesa in place of the Mali blob, and the Qt eglfs
    # integration that can drive it, since this Qt was built with only the blob's.
    # See build_mali_mesa.sh and build_qt5_eglfs_kms.sh.
    "$SCRIPT_DIR_SELF/build_mali_mesa.sh"
    cp -a "$REPO_ROOT/build/mesa-$MESA_VERSION-armhf-mali" "$STAGE_DIR/mesa-mali"
    # The plugin is checked against this rootfs's own Qt, read out of the image
    # the way detect_mesa.sh reads it.
    DEBUGFS=""
    for c in debugfs /sbin/debugfs /usr/sbin/debugfs /opt/homebrew/opt/e2fsprogs/sbin/debugfs; do
        if command -v "$c" >/dev/null 2>&1; then DEBUGFS="$c"; break; fi
    done
    mkdir "$STAGE_DIR/vendor-qt"
    for so in libQt5Core libQt5Gui libQt5EglFSDeviceIntegration; do
        "$DEBUGFS" -R "dump /usr/qt/lib/$so.so.5.15.2 $STAGE_DIR/vendor-qt/$so.so.5.15.2" \
            "$OUT_PATH" >/dev/null 2>&1
        [ -s "$STAGE_DIR/vendor-qt/$so.so.5.15.2" ] || {
            echo "ERROR: no /usr/qt/lib/$so.so.5.15.2 in this rootfs; only Qt 5.15.2" >&2
            echo "       has an eglfs_kms build here." >&2; exit 1; }
    done
    # Engine's Qt has qreal as double, HeadRush's as float; QWindow::setOpacity's
    # mangled name says which.
    READELF="$(command -v readelf || echo /opt/homebrew/opt/binutils/bin/readelf)"
    QREAL=double
    if "$READELF" --dyn-syms -W "$STAGE_DIR/vendor-qt/libQt5Gui.so.5.15.2" |
            grep '_ZN7QWindow10setOpacityEf' >/dev/null; then
        QREAL=float
    fi
    "$SCRIPT_DIR_SELF/build_qt5_eglfs_kms.sh" --qreal "$QREAL" --check-against "$STAGE_DIR/vendor-qt"
    rm -rf "$STAGE_DIR/vendor-qt"
    QT_KMS="$REPO_ROOT/build/qt5-eglfs-kms-5.15.2-armhf"
    [ "$QREAL" = double ] || QT_KMS="$QT_KMS-qreal-$QREAL"
    cp -a "$QT_KMS" "$STAGE_DIR/qt5-eglfs-kms"

    # HeadRush only: qmlrotate, which links Qt and so needs Debian's Qt5 headers,
    # an install every Engine build would otherwise pay for. It uses no API that
    # passes a qreal, so Debian's qreal-double headers suit HeadRush's float Qt.
    if "$DEBUGFS" -R 'stat /usr/Evil/Evil' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:'; then
        echo "--- building qmlrotate ---"
        docker run --rm --platform linux/arm/v7 \
            -v "$SHIMS_DIR:/shims:ro" -v "$STAGE_DIR:/stage" \
            debian:bookworm bash -c '
                set -e
                export DEBIAN_FRONTEND=noninteractive
                apt-get update -qq
                apt-get install -y -qq --no-install-recommends g++ pkg-config qtbase5-dev >/dev/null 2>&1
                g++ -shared -fPIC -O2 -Wall -std=c++17 -o /stage/qmlrotate.so \
                    /shims/qmlrotate/qmlrotate.cpp $(pkg-config --cflags --libs Qt5Gui)'
        [ -s "$STAGE_DIR/qmlrotate.so" ] || {
            echo "ERROR: qmlrotate did not build." >&2; exit 1; }
    fi
else
    "$SCRIPT_DIR_SELF/build_virgl_mesa.sh" --arch "$SHIM_ARCH" \
        --mesa-version "$MESA_VERSION" --layout "$MESA_LAYOUT"
    case "$MESA_LAYOUT" in
        gallium) VIRGL_ARTIFACT="libgallium-$MESA_VERSION.so"
                 VIRGL_BUILT="$REPO_ROOT/build/libgallium-$MESA_VERSION-$SHIM_ARCH.so" ;;
        dri)     VIRGL_ARTIFACT="virtio_gpu_dri.so"
                 VIRGL_BUILT="$REPO_ROOT/build/virtio_gpu_dri-$MESA_VERSION-$SHIM_ARCH.so" ;;
    esac
    [ -s "$VIRGL_BUILT" ] || {
        echo "ERROR: no $(basename "$VIRGL_BUILT") in build/." >&2
        exit 1
    }
    cp -a "$VIRGL_BUILT" "$STAGE_DIR/$VIRGL_ARTIFACT"
fi

# The shared shims are checked separately: they live outside SHIMS_DIR, and each
# one is named for the architecture it was built for rather than for a device.
for artifact in alsashim/alsashim_$SHIM_ARCH.so \
                quitshim/quitshim_$SHIM_ARCH.so \
                drmatomic/drmatomic_$SHIM_ARCH.so \
                touchbridge/touchbridge_$SHIM_ARCH \
                rk3288/vnctouchbridge/vnctouchbridge_$SHIM_ARCH \
                midisurface/midisurface_$SHIM_ARCH \
                pcmhold/pcmhold_$SHIM_ARCH \
                dtshim/dtshim_$SHIM_ARCH.so; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

fi # modern shim and graphics stack

### 3-5. e2fsck/resize2fs + telemetry block + shims + engine.service, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-armv7-engine-rootfs-inner.XXXXXX.sh)"
trap 'rm -rf "$STAGE_DIR"; rm -f "$INNER_SCRIPT"' EXIT

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
    echo "       This builds armv7/RK3288 Engine OS firmware only." >&2
    exit 1
fi
# Engine, or HeadRush's Evil, which shares Engine 4.3.4's platform: the same
# Buildroot base, Mali blob, Qt 5.15.2, data disk and overlays. Told apart by the
# rootfs, as build_mpc_rootfs.sh tells MPC from the drum module.
if [ -d /mnt/rootfs/usr/Engine ]; then
    APP_UNIT=engine.service; DEFAULT_CODE=JP07
elif [ -x /mnt/rootfs/usr/JP07/JP07 ]; then
    APP_UNIT=jp07.service; DEFAULT_CODE=JP07
elif [ -x /mnt/rootfs/usr/Evil/Evil ]; then
    APP_UNIT=evil.service; DEFAULT_CODE=HG06
else
    echo "ERROR: neither /usr/Engine nor /usr/Evil in this rootfs." >&2
    exit 1
fi
PRODUCT_CODE="${PRODUCT_CODE:-$DEFAULT_CODE}"
echo "--- app unit $APP_UNIT, product code $PRODUCT_CODE ---"

block_telemetry /mnt/rootfs
blank_root_password /mnt/rootfs
if [ "$LEGACY_ENGINE" = 1 ]; then
    # -skipFirmwareUpdate does not exist in 1.x. Do not inject it into the old
    # launchers or install contemporary ALSA/MIDI/EGLFS preloads into this libc.
    write_fake_dt /mnt/rootfs "$PRODUCT_CODE"
    install_legacy_engine /mnt/rootfs /stage
else
# Engine's control-surface firmware check; Evil has no such flag to pass.
if [ "$APP_UNIT" = engine.service ]; then skip_firmware_update /mnt/rootfs; fi

install_virgl_mesa "$MESA_LAYOUT" "$MESA_VERSION" /mnt/rootfs /stage

#
# Nor is anything staged at Engine's hardcoded Mali eglfs-integration path,
# /usr/lib/qt6/plugins/egldeviceintegrations/libqeglfs-mali-integration.so, which
# Engine access()es and which is wrong even on real hardware (the real plugins live
# in /usr/lib/plugins/egldeviceintegrations, no qt6 segment). Satisfying it was
# tried, and Engine renders with the path absent: the failure it was suspected of
# causing was really Qt picking no device integration at all, which
# QT_QPA_EGLFS_INTEGRATION now settles.

echo "--- inserting shims into /root ---"
cp -a /shims/dtshim/dtshim_$SHIM_ARCH.so /mnt/rootfs/root/dtshim.so
cp -a /shims/drmatomic/drmatomic_$SHIM_ARCH.so /mnt/rootfs/root/drmatomic.so
cp -a /shims/touchbridge/touchbridge_$SHIM_ARCH /mnt/rootfs/root/touchbridge
cp -a /shims/alsashim/alsashim_$SHIM_ARCH.so /mnt/rootfs/root/alsashim.so
cp -a /shims/quitshim/quitshim_$SHIM_ARCH.so /mnt/rootfs/root/quitshim.so
# A service rather than a preload: it is a MIDI device Engine binds, not a
# library Engine loads. It reads /root/fake-dt/inmusic,product-code itself to
# decide which device to answer Engine's inquiry as, so one binary and one unit
# serve every product this image can be built as.
cp -a /shims/midisurface/midisurface_$SHIM_ARCH /mnt/rootfs/root/midisurface
# Not wired to a unit: it is started by hand from the prime4/primego launcher
# scripts in scripts/vm, which invoke /root/vnctouchbridge by that name. Installed
# unconditionally because it is 14KB and those scripts cannot work without it --
# until now they expected a copy nothing in the build produced.
cp -a /shims/rk3288/vnctouchbridge/vnctouchbridge_$SHIM_ARCH /mnt/rootfs/root/vnctouchbridge
chmod 755 /mnt/rootfs/root/dtshim.so \
          /mnt/rootfs/root/drmatomic.so \
          /mnt/rootfs/root/touchbridge \
          /mnt/rootfs/root/alsashim.so \
          /mnt/rootfs/root/quitshim.so \
          /mnt/rootfs/root/midisurface \
          /mnt/rootfs/root/vnctouchbridge

# The devicetree properties dtshim.c remaps. These are the real RK3288 paths;
# only the values are ours. Every one of them must exist, because the shim remaps
# unconditionally and a missing target turns a working read into ENOENT.
#
# PRODUCT_CODE selects which device this pretends to be.
write_fake_dt /mnt/rootfs "$PRODUCT_CODE"

echo "--- wiring touchbridge.service + $APP_UNIT override ---"
# The same unit either builder installs, each from its own SoC directory: the
# binary is shared but the invocation is not -- RK3588 passes --head N and has a
# templated unit per display, RK3288 is single-head and passes a resolution.
cp -a /shims/rk3288/touchbridge/touchbridge.service /mnt/rootfs/etc/systemd/system/touchbridge.service
ln -sf ../touchbridge.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge.service

# The virtual control surface. The same unit both builders install: it names
# no device and passes no client name, because the binary works out which
# product to be from the guest's own product code. Engine only: it answers
# Engine's KnownDevices inquiry, which Evil does not send.
if [ "$APP_UNIT" = engine.service ]; then
    cp -a /shims/midisurface/midisurface.service /mnt/rootfs/etc/systemd/system/midisurface.service
    ln -sf ../midisurface.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/midisurface.service
fi

echo "--- disabling the tty1 getty (Application's display) ---"
# Engine renders fullscreen via eglfs/KMS on the same VT the console getty
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

OVERRIDE="/mnt/rootfs/etc/systemd/system/$APP_UNIT.d/override.conf"
mkdir -p "$(dirname "$OVERRIDE")"
if [ "$APP_UNIT" = evil.service ]; then
    # Evil opens its own card by name, hw:HV01, and Prime 5.2.0 segfaults a few
    # seconds after startup when there is none. The kernel's loopback card stands
    # in: it needs no host audio, and get_kernel.sh loads it from the initrd, which
    # is too early for a modprobe.d id= option on the rootfs to reach it, so its
    # id is renamed through sysfs instead before Evil starts. $$ is systemd's
    # escape for a literal $.
    #
    # The name alone is not enough. A loopback offers 32 channels each way, Evil
    # takes them all and then segfaults in its audio thread overrunning buffers
    # sized for the real card, which HG06's devicetree gives 8 playback and 2
    # capture channels. pcmhold pins the loopback to that shape; see pcmhold.c.
    cp -a /shims/pcmhold/pcmhold_$SHIM_ARCH /mnt/rootfs/root/pcmhold
    chmod 755 /mnt/rootfs/root/pcmhold
    cat > /mnt/rootfs/etc/systemd/system/qengine-hv01.service <<'EOF'
[Unit]
Description=Stand the loopback sound card in for HV01, the card Evil opens
Before=evil.service

[Service]
Type=simple
ExecStart=/root/pcmhold HV01 8 2
# Evil must not open the card before pcmhold has pinned it.
ExecStartPost=/bin/sleep 1
Restart=on-failure
ExecStartPre=/bin/sh -c 'for c in /sys/class/sound/card*; do [ "$$(cat $$c/id)" = Loopback ] && printf HV01 > $$c/id; done; grep -q "\\[HV01" /proc/asound/cards'

[Install]
WantedBy=multi-user.target
EOF
    ln -sf ../qengine-hv01.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/qengine-hv01.service

    # Evil turns its UI -90 degrees for HG06's portrait panel; qmlrotate turns it
    # back for QEngine's landscape screen. See qmlrotate.cpp.
    cp -a /stage/qmlrotate.so /mnt/rootfs/root/qmlrotate.so
    chmod 755 /mnt/rootfs/root/qmlrotate.so

    # dtshim for the product code, serial and pcb-rev Evil reads, and drmatomic for
    # virtio-gpu as below, and qmlrotate. Not alsashim or quitshim: both are
    # written around Engine's audio and Engine's quit. The preload reaches every
    # process runevil starts, from /bin/sh on, and qmlrotate links Qt, so
    # /usr/qt/lib has to be on the path from the unit, not only from Evil's own
    # script, which sets it later.
    cat > "$OVERRIDE" <<'EOF'
[Unit]
After=touchbridge.service qengine-hv01.service
Requires=touchbridge.service qengine-hv01.service

[Service]
Environment=LD_PRELOAD=/root/dtshim.so:/root/drmatomic.so:/root/qmlrotate.so
Environment=LD_LIBRARY_PATH=/usr/qt/lib
Environment=QMLROTATE=90
EOF
else
    cat > "$OVERRIDE" <<'EOF'
[Unit]
After=touchbridge.service midisurface.service
Requires=touchbridge.service
Wants=midisurface.service

[Service]
# quitshim: turns the SIGTERM that every stop, restart and poweroff sends Engine
# into Engine's own clean quit. Qt's eglfs handler would otherwise _exit(1) on
# the spot, leaving whatever Engine had open on the data disk mid-write. Also
# stands in for the firmware's splash service, which Engine aborts without on
# its way out. TimeoutStopSec=30 in the stock unit is the SIGKILL fallback.
Environment=LD_PRELOAD=/root/dtshim.so:/root/drmatomic.so:/root/alsashim.so:/root/quitshim.so
EOF
fi
# The display, the same for either app.
cat >> "$OVERRIDE" <<'EOF'
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
# virtio_gpu, so GL goes to the host's GPU through virgl rather than being
# rasterized on an emulated CPU. Two things had to land first: the machine gained
# working PCI when it moved to highmem=off, which is what lets virtio-gpu-gl-pci
# attach, and the staged armhf virtio_gpu_dri.so above is the driver Mesa needs to
# use it -- the vendor megadriver has no virgl in it.
#
# Named explicitly for the same reason kms_swrast was: Mesa's loader probes by the
# kernel's device name, and being definite makes the failure obvious if the driver
# ever goes missing. To go back to software, set this to kms_swrast -- still present
# in the image -- and use a non-GL display mode.
Environment=MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu
EOF

if [ "$MONTERO" = 1 ]; then
    install_montero /mnt/rootfs "$SHIM_ARCH" /stage
fi

fi # modern rootfs installation

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
    -e LEGACY_ENGINE="$LEGACY_ENGINE" \
    -e PRODUCT_CODE="${PRODUCT_CODE:-}" \
    -e MONTERO="$MONTERO" \
    -e MESA_LAYOUT="$MESA_LAYOUT" \
    -e MESA_VERSION="$MESA_VERSION" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
    -v "$STAGE_DIR:/stage:ro" \
    -v "$SCRIPT_DIR_SELF/rootfs_steps:/steps:ro" \
    -v "$REPO_ROOT/scripts/vm:/vm:ro" \
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