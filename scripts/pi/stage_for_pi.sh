#!/bin/bash
# stage_for_pi.sh — gathers everything a Raspberry Pi needs to run an instance's
# rootfs in a chroot, into one directory to copy across.
#
# Usage: stage_for_pi.sh --name <instance> [--out <dir>] [--wayland]
#   <instance> is one made by scripts/build_scripts/new_instance.sh; it must be an
#   Engine or MPC instance and must not be running. armv7 ones run as 32-bit
#   programs on the Pi's arm64 kernel. Writes
#   build/pi-stage/ by default. Requires Docker. An MPC instance goes into mpc/,
#   beside chroot-mpc.sh, kmsshim.so and kmspresent.
#
#   --wayland also stages what Engine needs to run under a Wayland compositor
#   (PLATFORM=wayland in chroot-engine.sh): a libEGL with the Wayland platform
#   from the same Mesa build, the libwayland libraries, and Qt's Wayland
#   plugin built by build_qtwayland.sh. eglfs keeps working with them in place.
#
# Then, for example:
#   rsync -a --sparse build/pi-stage/ pi@raspberrypi.local:qengine/
#   ssh pi@raspberrypi.local 'sudo qengine/chroot-up.sh &&
#       sudo qengine/install_into_rootfs.sh && sudo qengine/chroot-engine.sh'
#
# What is staged, and why the instance's rootfs is not enough on its own:
#   rootfs.img       the instance's, as built for QEMU
#   stage/           what a Pi needs that a guest does not, installed over the
#                    rootfs by install_into_rootfs.sh:
#     libgallium     Mesa with v3d and vc4 added -- the guest's has only virgl
#     shims          built from the working tree, so they carry whatever the
#                    rootfs builder does not install (cursorshim, quitshim) and any
#                    fix newer than the instance
#     controllermap  the script, manifest and mappings
#     libEGL, wayland/, qtwayland/   with --wayland only
#   chroot-up.sh, chroot-engine.sh, chroot-down.sh, powerkey.py,
#   install_into_rootfs.sh, sway.config, sway-split.config
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR="$REPO_ROOT/scripts/pi"
NAME=""
OUT="$REPO_ROOT/build/pi-stage"
WAYLAND=0
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --wayland) WAYLAND=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done
[ -n "$NAME" ] || { echo "ERROR: --name is required." >&2; exit 1; }
# Absolute, since it is handed to docker -v, which takes a relative path for a
# volume name.
mkdir -p "$OUT" && OUT="$(cd "$OUT" && pwd)"

INSTANCE_DIR="$REPO_ROOT/build/instances/$NAME"
[ -f "$INSTANCE_DIR/instance.env" ] || {
    echo "ERROR: no instance '$NAME' in build/instances." >&2; exit 1; }
# shellcheck disable=SC1091
. "$INSTANCE_DIR/instance.env"
case "${ARCH:-}" in arm64|armhf) ;; *)
    echo "ERROR: instance '$NAME' is ${ARCH:-unknown}, neither arm64 nor armhf." >&2; exit 1 ;;
esac
# A disk image copied while its guest has it mounted read-write is not a
# consistent filesystem.
if pgrep -f "qemu-system.*$INSTANCE_DIR/" >/dev/null 2>&1; then
    echo "ERROR: instance '$NAME' is running; shut it down first." >&2; exit 1
fi

# The image is mostly holes. Clone it where the filesystem can (macOS/APFS),
# keep it sparse where cp knows how (GNU), and only then copy every byte.
copy_image() {
    cp -c "$1" "$2" 2>/dev/null || cp --sparse=always "$1" "$2" 2>/dev/null || cp "$1" "$2"
}

# MPC needs its two disks and chroot-mpc.sh: it renders without a GPU, and its
# shims are already in the rootfs. Staged under mpc/, so that an Engine stage and
# an MPC one can share a directory. kmsshim.so and kmspresent are only for
# PLATFORM=wayland: the first is loaded into MPC, so it is built against the
# same old glibc as the other shims, and the second runs on the host, built
# there on bookworm so that any newer distribution can run it.
if [ "${DEVICE:-}" = mpc ]; then
    [ -n "${DATA_IMG:-}" ] && [ -f "$DATA_IMG" ] || {
        echo "ERROR: instance '$NAME' has no data disk." >&2; exit 1; }
    mkdir -p "$OUT/mpc"
    echo "--- building kmsshim and kmspresent ---"
    docker pull -q --platform linux/arm64 debian:bookworm >/dev/null
    docker run --rm --platform linux/arm64 \
        -v "$REPO_ROOT/shims:/shims:ro" -v "$OUT:/out" \
        debian:bookworm bash -c '
            set -e
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq gcc libc6-dev libdrm-dev libwayland-dev wayland-protocols \
                libwayland-bin libdecor-0-dev pkg-config >/dev/null 2>&1
            gcc -shared -fPIC -O2 -Wall -I/usr/include/libdrm \
                -o /out/kmsshim.so /shims/kmsshim/kmsshim.c -ldl -lpthread
            P=$(pkg-config --variable=pkgdatadir wayland-protocols)
            mkdir -p /tmp/proto && cd /tmp/proto
            wayland-scanner client-header "$P/stable/viewporter/viewporter.xml" viewporter-client-protocol.h
            wayland-scanner private-code "$P/stable/viewporter/viewporter.xml" viewporter.c
            gcc -O2 -Wall -Wno-missing-field-initializers -I. -I/shims/kmsshim $(pkg-config --cflags libdecor-0) \
                -o /out/kmspresent /shims/kmsshim/kmspresent.c viewporter.c -lwayland-client -ldecor-0
        '
    # Gen 1's MPC is a 32-bit program, so the kmsshim loaded into it is too.
    # kmspresent stays the host's.
    KMS_FILES="kmsshim.so kmspresent"
    if [ "$ARCH" = armhf ]; then
        KMS_FILES="$KMS_FILES kmsshim-armhf.so"
        docker pull -q --platform linux/arm/v7 debian:bookworm >/dev/null
        docker run --rm --platform linux/arm/v7 \
            -v "$REPO_ROOT/shims:/shims:ro" -v "$OUT:/out" \
            debian:bookworm bash -c '
                set -e
                export DEBIAN_FRONTEND=noninteractive
                apt-get update -qq
                apt-get install -y -qq gcc libc6-dev libdrm-dev >/dev/null 2>&1
                gcc -shared -fPIC -O2 -Wall -I/usr/include/libdrm \
                    -o /out/kmsshim-armhf.so /shims/kmsshim/kmsshim.c -ldl -lpthread
            '
    fi
    for f in $KMS_FILES; do
        [ -s "$OUT/$f" ] || { echo "ERROR: $f was not built." >&2; exit 1; }
    done
    echo "--- copying the rootfs and data disk ---"
    copy_image "$ROOTFS_IMG" "$OUT/mpc/rootfs.img"
    # data_disk.img on Gen 2, emmc.img on Gen 1, which chroot-mpc.sh tells apart.
    rm -f "$OUT/mpc/data_disk.img" "$OUT/mpc/emmc.img"
    copy_image "$DATA_IMG" "$OUT/mpc/$(basename "$DATA_IMG")"
    cp "$SCRIPT_DIR/chroot-mpc.sh" "$OUT/"
    chmod 755 "$OUT/chroot-mpc.sh"
    echo ""
    echo "Staged $NAME (${PRODUCT_CODE:-default product}) in $OUT/mpc"
    echo "Copy it across with something that keeps the images sparse, e.g."
    echo "  rsync -a --sparse $OUT/ <user>@<host>:qengine/"
    exit 0
fi

mkdir -p "$OUT/stage"

# shellcheck source=../build_scripts/detect_mesa.sh
. "$REPO_ROOT/scripts/build_scripts/detect_mesa.sh"
detect_mesa "$ROOTFS_IMG"
case "${MESA_LAYOUT:-}" in gallium|dri) ;; *)
    echo "ERROR: this rootfs's Mesa layout is '${MESA_LAYOUT:-unknown}'; see build_virgl_mesa.sh." >&2
    exit 1 ;;
esac
# A stage refreshed without --wayland must not install an earlier one's files,
# nor one for the other layout.
rm -rf "$OUT/stage/libEGL.so.1.0.0" "$OUT/stage/wayland" "$OUT/stage/qtwayland" \
    "$OUT/stage/dri" "$OUT/stage"/libgallium-*.so
# The gallium layout (arm64 5.x) replaces the vendor libgallium whole. The dri
# layout (armv7 5.3.0) loads /usr/lib/dri/<kernel driver>_dri.so, so the one
# megadriver is staged under the Pi's two names, v3d and vc4, beside the vendor's.
if [ "$MESA_LAYOUT" = gallium ]; then
    MESA_SO="libgallium-$MESA_VERSION-$ARCH+v3d+vc4"
else
    MESA_SO="virtio_gpu_dri-$MESA_VERSION-$ARCH+v3d+vc4"
fi
stage_mesa() {
    if [ "$MESA_LAYOUT" = gallium ]; then
        cp "$REPO_ROOT/build/$MESA_SO$1.so" "$OUT/stage/libgallium-$MESA_VERSION.so"
    else
        mkdir -p "$OUT/stage/dri"
        cp "$REPO_ROOT/build/$MESA_SO$1.so" "$OUT/stage/dri/v3d_dri.so"
        cp "$REPO_ROOT/build/$MESA_SO$1.so" "$OUT/stage/dri/vc4_dri.so"
    fi
}
if [ "$WAYLAND" -eq 1 ]; then
    # The libEGL and the driver come from one build, so they go in together.
    "$REPO_ROOT/scripts/build_scripts/build_virgl_mesa.sh" --arch "$ARCH" \
        --mesa-version "$MESA_VERSION" --layout "$MESA_LAYOUT" --extra-drivers v3d,vc4 \
        --platforms wayland
    stage_mesa +wayland
    cp "$REPO_ROOT/build/libEGL-$MESA_VERSION-$ARCH+v3d+vc4+wayland.so" \
       "$OUT/stage/libEGL.so.1.0.0"
    cp -R "$REPO_ROOT/build/wayland-$ARCH" "$OUT/stage/wayland"

    # Qt's Wayland plugin, for the rootfs's own Qt, and checked against it.
    # Read out of the image the way detect_mesa.sh does it.
    DEBUGFS=""
    for c in debugfs /sbin/debugfs /usr/sbin/debugfs /opt/homebrew/opt/e2fsprogs/sbin/debugfs; do
        if command -v "$c" >/dev/null 2>&1; then DEBUGFS="$c"; break; fi
    done
    QT_LIBS="$(mktemp -d /tmp/qengine-qt.XXXXXX)"
    trap 'rm -rf "$QT_LIBS"' EXIT
    QT_CORE="$("$DEBUGFS" -R 'ls -p /usr/lib' "$ROOTFS_IMG" 2>/dev/null |
        tr '/' ' ' | awk '$5 ~ /^libQt6Core\.so\.6\.[0-9]+\.[0-9]+$/ {print $5; exit}')"
    [ -n "$QT_CORE" ] || {
        echo "ERROR: no Qt 6 in this rootfs's /usr/lib; --wayland needs one." >&2; exit 1; }
    QT_VERSION="${QT_CORE#libQt6Core.so.}"
    for m in Core Gui OpenGL DBus; do
        "$DEBUGFS" -R "dump /usr/lib/libQt6$m.so.$QT_VERSION $QT_LIBS/libQt6$m.so.$QT_VERSION" \
            "$ROOTFS_IMG" 2>/dev/null
    done
    # The keyboard goes through the rootfs's own libxkbcommon. By its real name:
    # dumping the libxkbcommon.so.0 symlink would give the link, not the library.
    XKB="$("$DEBUGFS" -R 'ls -p /usr/lib' "$ROOTFS_IMG" 2>/dev/null |
        tr '/' ' ' | awk '$5 ~ /^libxkbcommon\.so\.0\.[0-9.]+$/ {print $5; exit}')"
    [ -n "$XKB" ] || {
        echo "ERROR: no libxkbcommon in this rootfs; the Wayland keyboard needs it." >&2; exit 1; }
    "$DEBUGFS" -R "dump /usr/lib/$XKB $QT_LIBS/$XKB" "$ROOTFS_IMG" 2>/dev/null
    "$REPO_ROOT/scripts/build_scripts/build_qtwayland.sh" --arch "$ARCH" --qt-version "$QT_VERSION" \
        --check-against "$QT_LIBS"
    cp -R "$REPO_ROOT/build/qtwayland-$QT_VERSION-$ARCH" "$OUT/stage/qtwayland"
else
    "$REPO_ROOT/scripts/build_scripts/build_virgl_mesa.sh" --arch "$ARCH" \
        --mesa-version "$MESA_VERSION" --layout "$MESA_LAYOUT" --extra-drivers v3d,vc4
    stage_mesa ""
fi

echo "--- building shims from source ---"
# debian:bookworm for a glibc older than the rootfs's, as in the rootfs builders.
# armv7 Engine is RK3288's, and has no TEE gate for teeshim to answer.
if [ "$ARCH" = armhf ]; then
    SHIM_PLATFORM=linux/arm/v7; SHIM_MACHINE="armv7l|armv8l"; SOC=SOC_RK3288; TEESHIM=0
else
    SHIM_PLATFORM=linux/arm64; SHIM_MACHINE="aarch64|arm64"; SOC=SOC_RK3588; TEESHIM=1
fi
rm -f "$OUT/stage/teeshim.so"
docker pull -q --platform "$SHIM_PLATFORM" debian:bookworm >/dev/null
docker run --rm --platform "$SHIM_PLATFORM" \
    -e SHIM_MACHINE="$SHIM_MACHINE" -e SOC="$SOC" -e TEESHIM="$TEESHIM" \
    -v "$REPO_ROOT/shims:/shims:ro" -v "$OUT/stage:/out" \
    debian:bookworm bash -c '
        set -e
        uname -m | grep -qxE "$SHIM_MACHINE" || {
            echo "ERROR: shim container is $(uname -m), expected $SHIM_MACHINE." >&2; exit 1; }
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1
        gcc -shared -fPIC -O2 -Wall -o /out/dtshim.so /shims/dtshim/dtshim.c -D$SOC -ldl -lpthread
        gcc -shared -fPIC -O2 -Wall -o /out/alsashim.so /shims/alsashim/alsashim.c -ldl
        [ "$TEESHIM" = 0 ] || gcc -shared -fPIC -O2 -Wall -o /out/teeshim.so /shims/teeshim/teeshim.c
        gcc -shared -fPIC -O2 -Wall -o /out/quitshim.so /shims/quitshim/quitshim.c -ldl -lpthread
        gcc -shared -fPIC -O2 -Wall -I/usr/include/libdrm \
            -o /out/cursorshim.so /shims/cursorshim/cursorshim.c -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /out/drmatomic.so /shims/drmatomic/drmatomic.c -ldl
        gcc -O2 -Wall -o /out/touchbridge /shims/touchbridge/touchbridge.c
        gcc -O2 -Wall -o /out/midisurface /shims/midisurface/midisurface.c -lasound
    ' 2>&1 | grep -v "Wnonnull-compare\|^ *[0-9]* |\|^ *|\|In function" || true
for f in dtshim.so alsashim.so cursorshim.so quitshim.so drmatomic.so touchbridge midisurface; do
    [ -s "$OUT/stage/$f" ] || { echo "ERROR: $f was not built." >&2; exit 1; }
done
[ "$TEESHIM" = 0 ] || [ -s "$OUT/stage/teeshim.so" ] || { echo "ERROR: teeshim.so was not built." >&2; exit 1; }

echo "--- staging controllermap and the Pi scripts ---"
rm -rf "$OUT/stage/controllermap"
cp -R "$REPO_ROOT/shims/rk3588/controllermap" "$OUT/stage/controllermap"
rm -f "$OUT/stage/controllermap/controllermap.service"
cp "$SCRIPT_DIR/chroot-up.sh" "$SCRIPT_DIR/chroot-engine.sh" "$SCRIPT_DIR/chroot-down.sh" \
   "$SCRIPT_DIR/powerkey.py" "$SCRIPT_DIR/install_into_rootfs.sh" \
   "$SCRIPT_DIR/qengine.service" "$SCRIPT_DIR/sway.config" \
   "$SCRIPT_DIR/sway-split.config" "$OUT/"

echo "--- copying the rootfs image ---"
copy_image "$ROOTFS_IMG" "$OUT/rootfs.img"

echo ""
echo "Staged $NAME (${PRODUCT_CODE:-default product}) in $OUT"
echo "Copy it to the Pi with something that keeps the image sparse, e.g."
echo "  rsync -a --sparse $OUT/ <user>@<pi>:qengine/"
