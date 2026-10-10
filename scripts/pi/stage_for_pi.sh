#!/bin/bash
# stage_for_pi.sh — gathers everything a Raspberry Pi needs to run an instance's
# rootfs in a chroot, into one directory to copy across.
#
# Usage: stage_for_pi.sh --name <instance> [--out <dir>]
#   <instance> is one made by scripts/build_scripts/new_instance.sh; it must be an
#   arm64 Engine instance and must not be running. Writes build/pi-stage/ by
#   default. Requires Docker.
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
#   chroot-up.sh, chroot-engine.sh, chroot-down.sh, powerkey.py,
#   install_into_rootfs.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR="$REPO_ROOT/scripts/pi"
NAME=""
OUT="$REPO_ROOT/build/pi-stage"
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done
[ -n "$NAME" ] || { echo "ERROR: --name is required." >&2; exit 1; }

INSTANCE_DIR="$REPO_ROOT/build/instances/$NAME"
[ -f "$INSTANCE_DIR/instance.env" ] || {
    echo "ERROR: no instance '$NAME' in build/instances." >&2; exit 1; }
# shellcheck disable=SC1091
. "$INSTANCE_DIR/instance.env"
[ "${ARCH:-}" = arm64 ] || {
    echo "ERROR: instance '$NAME' is ${ARCH:-unknown}; only arm64 has been run on a Pi." >&2
    exit 1; }
# A disk image copied while its guest has it mounted read-write is not a
# consistent filesystem.
if pgrep -f "qemu-system.*$INSTANCE_DIR/" >/dev/null 2>&1; then
    echo "ERROR: instance '$NAME' is running; shut it down first." >&2; exit 1
fi

mkdir -p "$OUT/stage"

# shellcheck source=../build_scripts/detect_mesa.sh
. "$REPO_ROOT/scripts/build_scripts/detect_mesa.sh"
detect_mesa "$ROOTFS_IMG"
[ "${MESA_LAYOUT:-}" = gallium ] || {
    echo "ERROR: this rootfs's Mesa layout is '${MESA_LAYOUT:-unknown}'; only the" >&2
    echo "       gallium layout can take the Pi's drivers (see build_virgl_mesa.sh)." >&2
    exit 1; }
"$REPO_ROOT/scripts/build_scripts/build_virgl_mesa.sh" --arch arm64 \
    --mesa-version "$MESA_VERSION" --layout gallium --extra-drivers v3d,vc4
cp "$REPO_ROOT/build/libgallium-$MESA_VERSION-arm64+v3d+vc4.so" \
   "$OUT/stage/libgallium-$MESA_VERSION.so"

echo "--- building shims from source ---"
# debian:bookworm for a glibc older than the rootfs's, as in the rootfs builders.
docker pull -q --platform linux/arm64 debian:bookworm >/dev/null
docker run --rm --platform linux/arm64 \
    -v "$REPO_ROOT/shims:/shims:ro" -v "$OUT/stage:/out" \
    debian:bookworm bash -c '
        set -e
        case "$(uname -m)" in aarch64|arm64) ;; *)
            echo "ERROR: shim container is $(uname -m), expected aarch64." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1
        gcc -shared -fPIC -O2 -Wall -o /out/dtshim.so /shims/dtshim/dtshim.c -DSOC_RK3588 -ldl -lpthread
        gcc -shared -fPIC -O2 -Wall -o /out/alsashim.so /shims/alsashim/alsashim.c -ldl
        gcc -shared -fPIC -O2 -Wall -o /out/teeshim.so /shims/teeshim/teeshim.c
        gcc -shared -fPIC -O2 -Wall -o /out/quitshim.so /shims/quitshim/quitshim.c -ldl -lpthread
        gcc -shared -fPIC -O2 -Wall -I/usr/include/libdrm \
            -o /out/cursorshim.so /shims/cursorshim/cursorshim.c -ldl -lpthread
        gcc -shared -fPIC -O2 -I/usr/include/libdrm \
            -o /out/drmatomic.so /shims/drmatomic/drmatomic.c -ldl
        gcc -O2 -Wall -o /out/touchbridge /shims/touchbridge/touchbridge.c
        gcc -O2 -Wall -o /out/midisurface /shims/midisurface/midisurface.c -lasound
    ' 2>&1 | grep -v "Wnonnull-compare\|^ *[0-9]* |\|^ *|\|In function" || true
for f in dtshim.so alsashim.so teeshim.so cursorshim.so quitshim.so drmatomic.so touchbridge midisurface; do
    [ -s "$OUT/stage/$f" ] || { echo "ERROR: $f was not built." >&2; exit 1; }
done

echo "--- staging controllermap and the Pi scripts ---"
rm -rf "$OUT/stage/controllermap"
cp -R "$REPO_ROOT/shims/rk3588/controllermap" "$OUT/stage/controllermap"
rm -f "$OUT/stage/controllermap/controllermap.service"
cp "$SCRIPT_DIR/chroot-up.sh" "$SCRIPT_DIR/chroot-engine.sh" "$SCRIPT_DIR/chroot-down.sh" \
   "$SCRIPT_DIR/powerkey.py" "$SCRIPT_DIR/install_into_rootfs.sh" \
   "$SCRIPT_DIR/qengine.service" "$OUT/"

echo "--- copying the rootfs image ---"
# The image is mostly holes. Clone it where the filesystem can (macOS/APFS),
# keep it sparse where cp knows how (GNU), and only then copy every byte.
cp -c "$ROOTFS_IMG" "$OUT/rootfs.img" 2>/dev/null ||
    cp --sparse=always "$ROOTFS_IMG" "$OUT/rootfs.img" 2>/dev/null ||
    cp "$ROOTFS_IMG" "$OUT/rootfs.img"

echo ""
echo "Staged $NAME (${PRODUCT_CODE:-default product}) in $OUT"
echo "Copy it to the Pi with something that keeps the image sparse, e.g."
echo "  rsync -a --sparse $OUT/ <user>@<pi>:qengine/"
