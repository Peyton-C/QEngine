#!/bin/bash
# build_qtwayland.sh — builds Qt's Wayland platform plugin for an Engine OS rootfs,
# which ships Qt 6 with eglfs only, so that Engine can run as a window under a
# Wayland compositor (sway, on a Raspberry Pi) instead of owning the display.
#
# Usage: build_qtwayland.sh --qt-version <ver> [--check-against <dir>] [--force]
#   Writes build/qtwayland-<ver>-arm64/: libQt6WaylandClient and its EGL
#   integration library, and the plugins they load (plugins/platforms,
#   plugins/wayland-*), laid out as under the rootfs's /usr/lib. Cached like
#   build_virgl_mesa.sh's output; --force rebuilds. Requires Docker, and runs an
#   arm64 container: native on Apple silicon, emulated (slow) elsewhere.
#
# --check-against names a directory holding the rootfs's own libQt6Core, Gui,
# OpenGL and DBus (*.so.6.x.y), and libxkbcommon.so.0*. Every Qt and xkbcommon
# symbol the result imports must be defined there or in the result itself, or
# Engine would fail to load the plugin -- the check that found what follows.
#
# Why qtbase is built too, and configured the way it is: the client is built
# against Qt's private headers, whose classes change layout with QtGui's
# configuration, so it has to see a qtbase configured like Engine's. That
# configuration was read off the vendor libQt6Gui's exports: GLES2, and cursor,
# sessionmanager, wayland, glib, xkbcommon, cssparser, pdf, picture,
# systemtrayicon and whatsthis all off. Only the client is kept; Engine goes on
# using its own qtbase.
#
# The client then needs patches/qtwayland-<ver>-engine.patch, whose header
# explains each change. In short: QtGui's wayland and xkbcommon features are
# off, so the client carries the three native interfaces and QtGui's xkbcommon
# helper itself, and both features are turned on in the installed headers only;
# a few places did not build without cursor; QT_WAYLAND_MOUSE_AS_TOUCH=1 turns
# the left mouse button into touch; and QT_WAYLAND_SCREEN_SIZE sets the screen
# size Engine lays out for.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

QT_VER=""
CHECK_DIR=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --qt-version) QT_VER="$2"; shift 2 ;;
        --check-against) CHECK_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done
[ -n "$QT_VER" ] || { echo "ERROR: --qt-version is required." >&2; exit 1; }
PATCH="$REPO_ROOT/scripts/build_scripts/patches/qtwayland-$QT_VER-engine.patch"
[ -f "$PATCH" ] || {
    echo "ERROR: no patches/qtwayland-$QT_VER-engine.patch; only Qt versions with" >&2
    echo "       one have been built." >&2; exit 1; }

OUT="$OUT_DIR/qtwayland-$QT_VER-arm64"
if [ -s "$OUT/libQt6WaylandClient.so.$QT_VER" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    exit 0
fi

echo "--- building QtWaylandClient $QT_VER for arm64 ---"
docker pull -q --platform linux/arm64 debian:bookworm >/dev/null

STAGE="$(mktemp -d /tmp/build-qtwayland.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
cp "$PATCH" "$STAGE/engine.patch"

# debian:bookworm for a glibc and libstdc++ older than the rootfs's, as for the
# shims.
docker run --rm --platform linux/arm64 -e QT_VER="$QT_VER" -v "$STAGE:/out" \
    ${CHECK_DIR:+-v "$CHECK_DIR:/engine-qt:ro"} \
    debian:bookworm bash -euo pipefail -c '
        case "$(uname -m)" in aarch64|arm64) ;; *)
            echo "ERROR: builder is $(uname -m), expected aarch64." >&2; exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential cmake ninja-build pkg-config python3 perl xz-utils \
            ca-certificates curl patch binutils \
            libgles-dev libegl-dev libdrm-dev libgbm-dev \
            libwayland-dev libwayland-bin wayland-protocols \
            libfontconfig-dev libfreetype-dev libharfbuzz-dev libpng-dev \
            libdbus-1-dev libudev-dev libzstd-dev zlib1g-dev libpcre2-dev \
            libxkbcommon-dev >/dev/null

        SERIES="${QT_VER%.*}"
        mkdir /b && cd /b
        for m in qtbase qtwayland; do
            curl -fsSL "https://download.qt.io/archive/qt/$SERIES/$QT_VER/submodules/$m-everywhere-src-$QT_VER.tar.xz" |
                tar xJ
            mv "$m-everywhere-src-$QT_VER" "$m"
        done

        OFF="cursor sessionmanager wayland glib xkbcommon cssparser pdf picture
             systemtrayicon whatsthis xcb vulkan icu sql libinput"
        FEATURES=""
        for f in $OFF; do FEATURES="$FEATURES -DFEATURE_$f=OFF"; done
        mkdir qtbase-build && cd qtbase-build
        cmake -G Ninja ../qtbase -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/qt \
            -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DINPUT_opengl=es2 \
            $FEATURES > /out/qtbase-cmake.log 2>&1 || {
                echo "ERROR: qtbase configure failed" >&2; tail -30 /out/qtbase-cmake.log >&2; exit 1; }
        ninja > /out/qtbase-ninja.log 2>&1 || {
            echo "ERROR: qtbase build failed" >&2; tail -30 /out/qtbase-ninja.log >&2; exit 1; }
        ninja install > /dev/null
        cd /b

        # The client will not configure, or compile, against a QtGui without the
        # wayland feature, and drops every key without the xkbcommon one. Both
        # are turned on in the installed headers and feature lists only: the
        # binaries stay as Engine has them, and the client carries what the
        # features would have added to QtGui (see the patch). Neither changes
        # the layout of any QtGui class.
        T=/opt/qt/lib/cmake/Qt6Gui/Qt6GuiTargets.cmake
        sed -i "s/#define QT_FEATURE_wayland -1/#define QT_FEATURE_wayland 1/" \
            /opt/qt/include/QtGui/qtgui-config.h
        sed -i -E "/QT_DISABLED_PUBLIC_FEATURES/s/;wayland\"/\"/; /QT_ENABLED_PUBLIC_FEATURES/s/\"\$/;wayland\"/" $T
        sed -i "s/#define QT_FEATURE_xkbcommon -1/#define QT_FEATURE_xkbcommon 1/" \
            /opt/qt/include/QtGui/$QT_VER/QtGui/private/qtgui-config_p.h
        sed -i -E "/QT_DISABLED_PRIVATE_FEATURES/s/;xkbcommon;/;/; /QT_ENABLED_PRIVATE_FEATURES/s/\"\$/;xkbcommon\"/" $T
        cp qtbase/src/gui/platform/unix/qxkbcommon_p.h /opt/qt/include/QtGui/$QT_VER/QtGui/private/

        cd qtwayland
        patch -p1 -s < /out/engine.patch
        cp ../qtbase/src/gui/platform/unix/qxkbcommon.cpp \
           ../qtbase/src/gui/platform/unix/qxkbcommon_3rdparty.cpp \
           ../qtbase/src/gui/platform/unix/qxkbcommon_p.h src/client/
        QI=/opt/qt/include
        /opt/qt/libexec/moc -I$QI -I$QI/QtCore -I$QI/QtGui \
            -I$QI/QtCore/$QT_VER -I$QI/QtCore/$QT_VER/QtCore \
            -I$QI/QtGui/$QT_VER -I$QI/QtGui/$QT_VER/QtGui \
            -f"qpa/qplatformwindow_p.h" $QI/QtGui/$QT_VER/QtGui/qpa/qplatformwindow_p.h \
            -o src/client/qtgui_wayland_iface_moc.cpp
        mkdir ../qtwayland-build && cd ../qtwayland-build
        /opt/qt/bin/qt-cmake -G Ninja ../qtwayland -DCMAKE_BUILD_TYPE=Release \
            -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF \
            -DFEATURE_wayland_server=OFF -DFEATURE_wayland_client=ON \
            > /out/qtwayland-cmake.log 2>&1 || {
                echo "ERROR: qtwayland configure failed" >&2; tail -30 /out/qtwayland-cmake.log >&2; exit 1; }
        ninja > /out/qtwayland-ninja.log 2>&1 || {
            echo "ERROR: qtwayland build failed" >&2; tail -30 /out/qtwayland-ninja.log >&2; exit 1; }
        DESTDIR=/b/install ninja install > /dev/null

        # Only what a client needs. wl-shell is deprecated and needs a library
        # of its own; sway and every current compositor speak xdg-shell.
        mkdir -p /out/result/plugins
        cp -a /b/install/opt/qt/lib/libQt6WaylandClient.so.6* \
              /b/install/opt/qt/lib/libQt6WaylandEglClientHwIntegration.so.6* /out/result/
        cp -a /b/install/opt/qt/plugins/platforms /b/install/opt/qt/plugins/wayland-* \
              /out/result/plugins/
        rm -f /out/result/plugins/wayland-shell-integration/libwl-shell-plugin.so

        [ -d /engine-qt ] || exit 0
        cd /out/result
        nm -D --defined-only /engine-qt/libQt6*.so.6.* libQt6Wayland*.so.6.* |
            awk "NF==3 {print \$3}" | sed "s/@@/@/" | sort -u > /tmp/have
        for f in libQt6Wayland*.so.6.* plugins/*/*.so; do
            nm -D --undefined-only "$f" | awk "{print \$2}" | grep "@Qt_6" | sort -u |
                comm -23 - /tmp/have | sed "s|^|$f: |"
        done | c++filt > /out/missing.txt
        if [ -s /out/missing.txt ]; then
            echo "ERROR: these Qt symbols are missing from the rootfs Qt:" >&2
            head -20 /out/missing.txt >&2; exit 1
        fi
        echo "--- every Qt symbol resolves against the rootfs Qt ---"
        XKB="$(ls /engine-qt/libxkbcommon.so.0* 2>/dev/null | head -n 1)"
        [ -n "$XKB" ] || { echo "ERROR: no libxkbcommon.so.0 to check against." >&2; exit 1; }
        nm -D --defined-only "$XKB" | awk "NF==3 {print \$3}" | sed "s/@.*//" | sort -u > /tmp/xkb
        nm -D --undefined-only libQt6WaylandClient.so.6.* | awk "{print \$2}" |
            grep "^xkb_" | sed "s/@.*//" | sort -u | comm -23 - /tmp/xkb > /out/missing.txt
        if [ -s /out/missing.txt ]; then
            echo "ERROR: these xkbcommon symbols are missing from the rootfs one:" >&2
            cat /out/missing.txt >&2; exit 1
        fi
        echo "--- every xkbcommon symbol resolves against the rootfs one ---"
    '

[ -s "$STAGE/result/libQt6WaylandClient.so.$QT_VER" ] || {
    echo "ERROR: the build produced no libQt6WaylandClient." >&2; exit 1; }
rm -rf "$OUT"
mv "$STAGE/result" "$OUT"
echo ""
echo "Built: $OUT"
