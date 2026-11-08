#!/bin/bash
# build_qt5_eglfs_kms.sh — builds Qt 5.15.2's eglfs_kms device integration for the
# Qt that armv7 Engine OS 4.3.4 and earlier ships, so that Engine can draw through
# the Mesa build_mali_mesa.sh puts in place of the Mali blob.
#
# Usage: build_qt5_eglfs_kms.sh [--qreal double|float] [--check-against <dir>] [--force]
#   Writes build/qt5-eglfs-kms-5.15.2-armhf[-qreal-float]/: lib/libQt5EglFsKmsSupport.so.5 and
#   plugins/egldeviceintegrations/libqeglfs-kms-integration.so, laid out as under
#   the rootfs's /usr/qt. Cached; --force rebuilds. Requires Docker.
#
# --check-against names a directory holding the rootfs's own libQt5Core,
# libQt5Gui and libQt5EglFSDeviceIntegration (*.so.5.15.2). Every Qt symbol the
# result imports must be defined there or in the result, or Engine fails to load
# the plugin. That check is what found both adjustments below.
#
# Why: this Qt was configured for the blob, so its only EGL device integrations are
# eglfs_mali, which hands EGL a Mali fbdev_window that Mesa does not understand,
# and eglfs_emu. eglfs_kms, the one that drives KMS through gbm, was never built.
#
# Why all of qtbase is built: the plugin uses Qt's private headers, whose classes
# change layout with QtGui's configuration, so it has to be built in a qtbase
# configured like the rootfs's. Only the plugin and its support library are kept;
# Engine goes on using its own Qt. Two things had to match, both read off the
# vendor libraries' exports:
#   - cursor is off. QtGui exports no QCursor, and the feature also changes
#     QPlatformCursor's vtable, which the plugin's cursor subclasses.
#   - EGLNativeWindowType is struct fbdev_window *, as in Mali's eglplatform.h,
#     where Mesa's header has an integer. It is part of the mangled name of
#     QEglFSDeviceIntegration::destroyNativeWindow, so the build patches the
#     Khronos header. Both are pointer-sized, and Mesa treats the value as opaque.
# With both, the libQt5EglFSDeviceIntegration this builds exports the same symbols
# as the vendor's, apart from the FreeType it bundles.
#
# --qreal is the third, and differs between products: Engine's Qt has qreal as
# double, HeadRush's (Prime 5.2.0) as float, so QWindow::setOpacity is
# setOpacityEd in one and setOpacityEf in the other, and so is every other
# function taking a qreal. Read it off the vendor QtGui; the builder does.
#
# qglobal.h gets #include <limits>, which GCC 11 and later no longer pull in
# transitively; Qt fixed it in 5.15.3. Built on bookworm for the reasons
# build_mali_mesa.sh gives.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"
QT_VER=5.15.2

CHECK_DIR=""
QREAL=double
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --check-against) CHECK_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --qreal) QREAL="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

case "$QREAL" in
    double) OUT="$OUT_DIR/qt5-eglfs-kms-$QT_VER-armhf" ;;
    float)  OUT="$OUT_DIR/qt5-eglfs-kms-$QT_VER-armhf-qreal-float" ;;
    *) echo "ERROR: --qreal takes double or float." >&2; exit 1 ;;
esac
PLUGIN="plugins/egldeviceintegrations/libqeglfs-kms-integration.so"
SUPPORT="lib/libQt5EglFsKmsSupport.so.$QT_VER"

READELF="$(command -v readelf || echo /opt/homebrew/opt/binutils/bin/readelf)"
[ -x "$READELF" ] || { echo "ERROR: readelf not found." >&2; exit 1; }

# Every Qt symbol the plugin and its support library import has to be defined by
# the rootfs's Qt or by themselves. Defined symbols print as name@@node and
# imports as name@node, so both are brought to the second form.
check_against() {
    _defs="$(for f in "$CHECK_DIR"/libQt5Core.so.$QT_VER "$CHECK_DIR"/libQt5Gui.so.$QT_VER \
                      "$CHECK_DIR"/libQt5EglFSDeviceIntegration.so.$QT_VER \
                      "$1/$SUPPORT"; do
            "$READELF" --dyn-syms -W "$f" | awk '$7 != "UND" && NF >= 8 {print $8}'
        done | sed 's/@@/@/' | sort -u)"
    _missing="$(for f in "$1/$SUPPORT" "$1/$PLUGIN"; do
            "$READELF" --dyn-syms -W "$f" | awk '$7 == "UND" {print $8}'
        done | grep '@Qt_5' | sort -u | comm -23 - <(printf '%s\n' "$_defs"))"
    [ -z "$_missing" ] || {
        echo "ERROR: the rootfs's Qt does not define what the plugin imports:" >&2
        printf '%s\n' "$_missing" | sed 's/^/       /' >&2
        return 1; }
    echo "--- every Qt symbol the plugin imports is in the rootfs's Qt ---"
}

if [ -s "$OUT/$PLUGIN" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    [ -z "$CHECK_DIR" ] || check_against "$OUT"
    exit 0
fi

case "$(uname -m)" in
    arm64|aarch64) PLATFORM=linux/arm64 ;;
    *)             PLATFORM=linux/amd64 ;;
esac
echo "--- building Qt $QT_VER eglfs_kms for armhf, qreal $QREAL ---"
docker pull -q --platform "$PLATFORM" debian:bookworm >/dev/null

STAGE="$(mktemp -d /tmp/build-qt5-eglfs-kms.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

docker run --rm --platform "$PLATFORM" -e QT_VER="$QT_VER" -e QREAL="$QREAL" -v "$STAGE:/out" \
    debian:bookworm bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        dpkg --add-architecture armhf
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential crossbuild-essential-armhf pkg-config python3 perl \
            xz-utils ca-certificates curl \
            libdrm-dev:armhf libgbm-dev:armhf libegl-dev:armhf libgles-dev:armhf >/dev/null

        # The window type Mali declares, in the branch this build selects.
        sed -i "s/^typedef khronos_uintptr_t EGLNativeWindowType;\$/typedef struct fbdev_window *EGLNativeWindowType;/" \
            /usr/include/EGL/eglplatform.h
        grep "fbdev_window" /usr/include/EGL/eglplatform.h >/dev/null || {
            echo "ERROR: could not patch EGLNativeWindowType in eglplatform.h." >&2; exit 1; }

        V=${QT_VER%.*}
        curl -fsSL "https://download.qt.io/archive/qt/$V/$QT_VER/submodules/qtbase-everywhere-src-$QT_VER.tar.xz" \
            -o /qt.tar.xz
        mkdir /src && tar xf /qt.tar.xz -C /src --strip-components=1
        sed -i "s/^\(#  include <utility>\)\$/\1\n#  include <limits>/" /src/src/corelib/global/qglobal.h
        grep "include <limits>" /src/src/corelib/global/qglobal.h >/dev/null
        # 5.15.2 does not build with -qreal float: qBound gets a double and a float.
        sed -i "s/qBound(double(INT_MIN) + 0.1, scale \* data.number, double(INT_MAX))/qBound(double(INT_MIN) + 0.1, double(scale * data.number), double(INT_MAX))/" \
            /src/src/gui/text/qcssparser.cpp

        export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
        export PKG_CONFIG_SYSROOT_DIR=/
        mkdir /b && cd /b
        /src/configure -prefix /usr/qt -release -opensource -confirm-license \
            -device linux-generic-g++ -device-option CROSS_COMPILE=arm-linux-gnueabihf- \
            -opengl es2 -egl -eglfs -kms -gbm -no-feature-cursor -qreal "$QREAL" \
            -no-xcb -no-widgets -no-dbus -no-glib -no-icu -no-openssl -no-libinput \
            -no-pch -nomake examples -nomake tests -pkg-config > /out/configure.log 2>&1 || {
                echo "ERROR: configure failed" >&2; tail -30 /out/configure.log >&2; exit 1; }
        make -j"$(nproc)" -C /b sub-src > /out/make.log 2>&1 || {
            echo "ERROR: qtbase build failed" >&2
            grep -m5 -A6 "error:" /out/make.log >&2 || tail -30 /out/make.log >&2
            exit 1; }

        mkdir -p /out/kms/lib /out/kms/plugins/egldeviceintegrations
        cp -a /b/lib/libQt5EglFsKmsSupport.so.5* /out/kms/lib/
        cp -a /b/plugins/egldeviceintegrations/libqeglfs-kms-integration.so \
            /out/kms/plugins/egldeviceintegrations/
    '

for f in "$SUPPORT" "$PLUGIN"; do
    [ -s "$STAGE/kms/$f" ] || { echo "ERROR: the build produced no $f." >&2; exit 1; }
done
[ -z "$CHECK_DIR" ] || check_against "$STAGE/kms"

rm -rf "$OUT" && mv "$STAGE/kms" "$OUT"
echo ""
echo "Built: $OUT"
