#!/bin/bash
# build_qt5_qtwayland.sh — builds Qt 5.15.2's Wayland platform plugin for the Qt
# that armv7 Engine OS 4.3.4 and earlier ships, so that Engine can run as a window
# under a Wayland compositor (sway, on a Raspberry Pi). build_qtwayland.sh does the
# same for Qt 6 Engine.
#
# Usage: build_qt5_qtwayland.sh [--check-against <dir>] [--force]
#   Writes build/qt5-qtwayland-5.15.2-armhf/: lib/libQt5WaylandClient.so.5* and
#   the plugins it loads (plugins/platforms, plugins/wayland-*), laid out as under
#   the rootfs's /usr/qt. Cached; --force rebuilds. Requires Docker.
#
# --check-against names a directory holding the rootfs's own libQt5*.so.5.15.2.
# Every Qt symbol the result imports must be defined there or in the result, or
# Engine fails to load the plugin. That check is what found the configuration
# below.
#
# Cross-compiled, as build_qt5_eglfs_kms.sh is, so it takes minutes: the Qt 6
# client is built in an emulated armv7 container instead, which takes most of an
# hour. The client subclasses QtGui's platform classes, so qtbase is configured
# like the rootfs's, read off its exports: cursor, sessionmanager, systemtrayicon,
# cssparser, pdf, picture, whatsthis and textodfwriter off, system zlib (the
# vendor QtCore exports no z_ symbols), qreal double, and Mali's
# EGLNativeWindowType, as build_qt5_eglfs_kms.sh explains. With those, every
# symbol this QtGui exports is in the vendor's. Only the client and its plugins
# are kept; Engine goes on using its own Qt.
#
# The client then needs patches/qtwayland-5.15.2-engine.patch, whose header
# explains it: the left mouse button as touch, and a screen size to lay out for.
# It also links libwayland-client and libxkbcommon. The rootfs has the second;
# build_mali_mesa.sh --platforms wayland supplies the first.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"
QT_VER=5.15.2
OUT="$OUT_DIR/qt5-qtwayland-$QT_VER-armhf"
PATCH="$REPO_ROOT/scripts/build_scripts/patches/qtwayland-$QT_VER-engine.patch"

CHECK_DIR=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --check-against) CHECK_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

CLIENT="lib/libQt5WaylandClient.so.$QT_VER"
PLUGINS="plugins/platforms/libqwayland-egl.so
plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so
plugins/wayland-shell-integration/libxdg-shell.so
plugins/wayland-decoration-client/libbradient.so"

READELF="$(command -v readelf || echo /opt/homebrew/opt/binutils/bin/readelf)"
[ -x "$READELF" ] || { echo "ERROR: readelf not found." >&2; exit 1; }

# As build_qt5_eglfs_kms.sh's: defined symbols print as name@@node and imports as
# name@node, so both are brought to the second form.
check_against() {
    _defs="$(for f in "$CHECK_DIR"/libQt5*.so."$QT_VER" "$1/$CLIENT"; do
            "$READELF" --dyn-syms -W "$f" | awk '$7 != "UND" && NF >= 8 {print $8}'
        done | sed 's/@@/@/' | sort -u)"
    _missing="$(for f in "$1/$CLIENT" $(printf '%s\n' "$PLUGINS" | sed "s|^|$1/|"); do
            "$READELF" --dyn-syms -W "$f" | awk '$7 == "UND" {print $8}'
        done | grep '@Qt_5' | sort -u | comm -23 - <(printf '%s\n' "$_defs"))"
    [ -z "$_missing" ] || {
        echo "ERROR: the rootfs's Qt does not define what the client imports:" >&2
        printf '%s\n' "$_missing" | sed 's/^/       /' >&2
        return 1; }
    echo "--- every Qt symbol the client imports is in the rootfs's Qt ---"
}

if [ -s "$OUT/$CLIENT" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    [ -z "$CHECK_DIR" ] || check_against "$OUT"
    exit 0
fi

case "$(uname -m)" in
    arm64|aarch64) PLATFORM=linux/arm64 ;;
    *)             PLATFORM=linux/amd64 ;;
esac
echo "--- building QtWaylandClient $QT_VER for armhf ---"
docker pull -q --platform "$PLATFORM" debian:bookworm >/dev/null

STAGE="$(mktemp -d /tmp/build-qt5-qtwayland.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
cp "$PATCH" "$STAGE/engine.patch"

docker run --rm --platform "$PLATFORM" -e QT_VER="$QT_VER" -v "$STAGE:/out" \
    debian:bookworm bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        dpkg --add-architecture armhf
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential crossbuild-essential-armhf pkg-config python3 perl \
            xz-utils ca-certificates curl patch libwayland-bin wayland-protocols \
            libdrm-dev:armhf libgbm-dev:armhf libegl-dev:armhf libgles-dev:armhf \
            libwayland-dev:armhf libxkbcommon-dev:armhf zlib1g-dev:armhf >/dev/null

        sed -i "s/^typedef khronos_uintptr_t EGLNativeWindowType;\$/typedef struct fbdev_window *EGLNativeWindowType;/" \
            /usr/include/EGL/eglplatform.h
        grep "fbdev_window" /usr/include/EGL/eglplatform.h >/dev/null || {
            echo "ERROR: could not patch EGLNativeWindowType in eglplatform.h." >&2; exit 1; }

        V=${QT_VER%.*}
        for m in qtbase qtwayland; do
            curl -fsSL "https://download.qt.io/archive/qt/$V/$QT_VER/submodules/$m-everywhere-src-$QT_VER.tar.xz" |
                tar xJ -C /
            mv "/$m-everywhere-src-$QT_VER" "/$m"
        done
        sed -i "s/^\(#  include <utility>\)\$/\1\n#  include <limits>/" /qtbase/src/corelib/global/qglobal.h
        grep "include <limits>" /qtbase/src/corelib/global/qglobal.h >/dev/null
        (cd /qtwayland && patch -p1 -s < /out/engine.patch)

        export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
        export PKG_CONFIG_SYSROOT_DIR=/
        OFF="cursor sessionmanager systemtrayicon cssparser pdf picture whatsthis textodfwriter"
        FEATURES=""
        for f in $OFF; do FEATURES="$FEATURES -no-feature-$f"; done
        mkdir /b && cd /b
        /qtbase/configure -prefix /opt/qt5 -release -opensource -confirm-license \
            -device linux-generic-g++ -device-option CROSS_COMPILE=arm-linux-gnueabihf- \
            -opengl es2 -egl -eglfs -kms -gbm $FEATURES -system-zlib -qreal double -xkbcommon \
            -no-xcb -no-widgets -no-dbus -no-glib -no-icu -no-openssl -no-libinput \
            -no-pch -nomake examples -nomake tests -pkg-config > /out/configure.log 2>&1 || {
                echo "ERROR: qtbase configure failed" >&2; tail -30 /out/configure.log >&2; exit 1; }
        make -j"$(nproc)" sub-src > /out/make.log 2>&1 || {
            echo "ERROR: qtbase build failed" >&2
            grep -m5 -A6 "error:" /out/make.log >&2 || tail -30 /out/make.log >&2
            exit 1; }
        make install > /dev/null

        mkdir /w && cd /w
        /opt/qt5/bin/qmake /qtwayland > /out/qmake-wayland.log 2>&1
        make -j"$(nproc)" > /out/make-wayland.log 2>&1 || {
            echo "ERROR: qtwayland build failed" >&2
            grep -m5 -A6 "error:" /out/make-wayland.log >&2 || tail -30 /out/make-wayland.log >&2
            exit 1; }

        mkdir -p /out/wl/lib /out/wl/plugins
        cp -a /w/lib/libQt5WaylandClient.so.5* /out/wl/lib/
        for d in platforms wayland-graphics-integration-client wayland-shell-integration \
                 wayland-decoration-client; do
            mkdir -p "/out/wl/plugins/$d"
            cp -a /w/plugins/$d/*.so "/out/wl/plugins/$d/"
        done
    '

for f in "$CLIENT" $PLUGINS; do
    [ -s "$STAGE/wl/$f" ] || { echo "ERROR: the build produced no $f." >&2; exit 1; }
done
[ -z "$CHECK_DIR" ] || check_against "$STAGE/wl"

rm -rf "$OUT" && mv "$STAGE/wl" "$OUT"
echo ""
echo "Built: $OUT"
