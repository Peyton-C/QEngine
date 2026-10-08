#!/bin/bash
# install_into_rootfs.sh — installs what stage_for_pi.sh staged into the mounted
# Engine rootfs: the Pi's Mesa, the shims, and controllermap.
#
# Usage: sudo install_into_rootfs.sh
#   Run after chroot-up.sh, once per rootfs image, and again whenever the stage
#   directory is refreshed. Safe while Engine is running; it takes effect at the
#   next chroot-engine.sh.
#
# Environment: QENGINE_DIR, ENGINE_ROOT as in chroot-up.sh.
set -euo pipefail

QENGINE_DIR="${QENGINE_DIR:-$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/qengine}"
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
R="${ENGINE_ROOT:-/srv/engine}"
STAGE="$QENGINE_DIR/stage"

mountpoint -q "$R" || { echo "ERROR: run chroot-up.sh first." >&2; exit 1; }
[ -d "$STAGE" ] || { echo "ERROR: no stage directory at $STAGE." >&2; exit 1; }

# Mesa. The library is replaced whole and has to match the version the rootfs's
# libEGL links against by name, so only a file the rootfs already has is
# replaced; the vendor one is kept beside it the first time.
for so in "$STAGE"/libgallium-*.so; do
    [ -e "$so" ] || continue
    name="$(basename "$so")"
    [ -e "$R/usr/lib/$name" ] || {
        echo "ERROR: this rootfs has no /usr/lib/$name; the staged Mesa is for a" >&2
        echo "       different firmware." >&2; exit 1; }
    [ -e "$R/usr/lib/$name.vendor" ] || cp -a "$R/usr/lib/$name" "$R/usr/lib/$name.vendor"
    install -o 0 -g 0 -m 755 "$so" "$R/usr/lib/$name"
    echo "installed $name"
done

for f in dtshim.so alsashim.so teeshim.so cursorshim.so drmatomic.so touchbridge midisurface; do
    [ -e "$STAGE/$f" ] || continue
    install -o 0 -g 0 -m 755 "$STAGE/$f" "$R/root/$f"
    echo "installed $f"
done

if [ -d "$STAGE/controllermap" ]; then
    rm -rf "$R/root/controllermap"
    cp -R "$STAGE/controllermap" "$R/root/controllermap"
    chown -R 0:0 "$R/root/controllermap"
    chmod 755 "$R/root/controllermap/controllermap.sh"
    echo "installed controllermap"
fi
