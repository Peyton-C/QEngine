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

# Mesa in place of the Mali blob (armv7 4.3.4 and earlier), which the rootfs
# builder already installed for virgl: the same build with the Pi's drivers.
if [ -d "$STAGE/mali" ]; then
    [ -e "$R/usr/lib/libmali.so.14.0.vendor" ] || {
        echo "ERROR: this rootfs's Mali blob was never replaced; rebuild the instance." >&2; exit 1; }
    cp -af "$STAGE"/mali/lib*.so.* "$R/usr/lib/"
    cp -af "$STAGE"/mali/dri/*_dri.so "$R/usr/lib/dri/"
    chown -h 0:0 "$R"/usr/lib/libmali.so.14.0 "$R"/usr/lib/libEGL.so* "$R"/usr/lib/libGLES* \
        "$R"/usr/lib/libgbm.so* "$R"/usr/lib/libglapi.so* "$R"/usr/lib/dri/*_dri.so
    echo "installed Mesa in place of libmali, with $(cd "$STAGE/mali/dri" && echo *_dri.so | wc -w) drivers"
    # With --wayland: the libwayland its libEGL needs, and libffi where the
    # rootfs has none of its own.
    for so in "$STAGE"/mali/wayland/*.so.*; do
        [ -e "$so" ] || continue
        case "$(basename "$so")" in
            libffi.*) [ -e "$R/usr/lib/$(basename "$so")" ] && continue ;;
        esac
        install -o 0 -g 0 -m 755 "$so" "$R/usr/lib/$(basename "$so")"
    done
    [ ! -d "$STAGE/mali/wayland" ] || echo "installed libwayland"
fi

# Qt 5's Wayland plugin (4.3.4 and earlier), under Qt's prefix, /usr/qt.
if [ -d "$STAGE/qt5wayland" ]; then
    cp -a "$STAGE"/qt5wayland/lib/libQt5WaylandClient.so.5* "$R/usr/qt/lib/"
    chown -h 0:0 "$R"/usr/qt/lib/libQt5WaylandClient.so.5*
    for d in "$STAGE"/qt5wayland/plugins/*/; do
        d="$(basename "$d")"
        mkdir -p "$R/usr/qt/plugins/$d"
        install -o 0 -g 0 -m 755 "$STAGE/qt5wayland/plugins/$d"/*.so "$R/usr/qt/plugins/$d/"
    done
    echo "installed qt5wayland"
fi

# Mesa in the dri layout (armv7 5.3.0): the Pi's drivers, under the names its
# loader derives from the kernel's, beside the vendor megadriver, which stays.
for so in "$STAGE"/dri/*_dri.so; do
    [ -e "$so" ] || continue
    [ -d "$R/usr/lib/dri" ] || {
        echo "ERROR: this rootfs has no /usr/lib/dri; the staged Mesa is for a" >&2
        echo "       different firmware." >&2; exit 1; }
    install -o 0 -g 0 -m 755 "$so" "$R/usr/lib/dri/$(basename "$so")"
    echo "installed dri/$(basename "$so")"
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
