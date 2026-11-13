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
# Settings file first, then the caller's environment again on top of it, so that
# `sudo AUDIO_CARD=... chroot-engine.sh` overrides the file for one run.
_caller_env="$(export -p)"
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
eval "$_caller_env"
R="${ENGINE_ROOT:-/srv/engine}"
STAGE="$QENGINE_DIR/stage"

# Not "$R" itself, which chroot-up.sh binds before the rootfs goes on it.
mountpoint -q "$R/proc" || { echo "ERROR: run chroot-up.sh first." >&2; exit 1; }
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

# Wayland, from stage_for_pi.sh --wayland: a libEGL from the same Mesa build as
# the libgallium above, with the Wayland platform the vendor one lacks; the
# libwayland libraries no Engine OS rootfs has; and Qt's Wayland plugin, beside
# the rootfs's own Qt. None of it changes how Engine runs under eglfs.
if [ -e "$STAGE/libEGL.so.1.0.0" ]; then
    [ -e "$R/usr/lib/libEGL.so.1.0.0.vendor" ] ||
        cp -a "$R/usr/lib/libEGL.so.1.0.0" "$R/usr/lib/libEGL.so.1.0.0.vendor"
    install -o 0 -g 0 -m 755 "$STAGE/libEGL.so.1.0.0" "$R/usr/lib/libEGL.so.1.0.0"
    echo "installed libEGL.so.1.0.0"
fi
if [ -d "$STAGE/wayland" ]; then
    for so in "$STAGE"/wayland/*.so.*; do
        install -o 0 -g 0 -m 755 "$so" "$R/usr/lib/$(basename "$so")"
    done
    echo "installed libwayland"
fi
if [ -d "$STAGE/qtwayland" ]; then
    cp -a "$STAGE"/qtwayland/libQt6Wayland*.so.6* "$R/usr/lib/"
    chown -h 0:0 "$R"/usr/lib/libQt6Wayland*.so.6*
    for d in "$STAGE"/qtwayland/plugins/*/; do
        d="$(basename "$d")"
        mkdir -p "$R/usr/lib/plugins/$d"
        install -o 0 -g 0 -m 755 "$STAGE/qtwayland/plugins/$d"/*.so "$R/usr/lib/plugins/$d/"
    done
    echo "installed qtwayland"
fi

for f in dtshim.so alsashim.so teeshim.so cursorshim.so quitshim.so drmatomic.so touchbridge midisurface; do
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
