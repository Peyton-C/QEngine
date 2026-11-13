#!/bin/bash
# build_mali_mesa.sh — builds the Mesa that replaces the proprietary Mali blob in
# armv7 Engine OS 4.3.4 and earlier (and HeadRush), so that a guest renders through
# virgl instead of having no working GL at all.
#
# Usage: build_mali_mesa.sh [--extra-drivers <list>] [--platforms wayland] [--force]
#   Writes build/mesa-<ver>-armhf-mali/, laid out as the rootfs's /usr/lib:
#   libEGL, libGLESv1_CM, libGLESv2, libgbm and libglapi, dri/ with virtio_gpu,
#   kms_swrast and swrast, and a libmali.so.14.0 that stands in for the blob.
#   The version is MALI_MESA_VERSION from detect_mesa.sh. Cached like
#   build_virgl_mesa.sh's output; --force rebuilds. Requires Docker.
#
#   --extra-drivers adds gallium drivers for real hardware, `v3d,vc4` for a
#   Raspberry Pi, cached as mesa-<ver>-armhf-mali+v3d+vc4/. The whole set goes in
#   together on the Pi, since a DRI driver has to come from the same build as the
#   libglapi beside it.
#
#   --platforms wayland builds libEGL with the Wayland platform, for Engine as a
#   window (build_qt5_qtwayland.sh), cached with a +wayland suffix. It also
#   copies out the libwayland and libffi the rootfs lacks, into wayland/.
#
# Why a whole Mesa rather than a driver, as build_virgl_mesa.sh builds: these
# rootfs images have no Mesa to plug a driver into. libmali.so.14.0 provides EGL
# and GLES itself, libEGL.so and libGLESv2.so are symlinks to it, and there is no
# libgbm. The blob only drives a real Mali, so under QEMU nothing renders.
#
# Why the stand-in: the Qt libraries were linked against those symlinks, so every
# one records NEEDED libmali.so.14.0 by that name, down to the JPEG and SVG image
# plugins, which use no GL at all. The stand-in has that soname, no code, and
# NEEDED entries for Mesa's libEGL, libGLESv2 and libgbm, so whatever loads
# "libmali" gets Mesa, and the blob is never mapped. Earlier attempts left the blob
# in place beside a system Mesa, so a plugin that pulled it in put two EGL
# implementations in one process, which is the likeliest reason most icons never
# drew. What the Qt libraries import from libmali on Prime GO 4.3.0 and 4.3.4 is 40
# symbols, all core EGL 1.4 and GLES 2.0, so nothing Mali-specific is lost.
#
# Built on Debian bookworm, unlike build_virgl_mesa.sh's trixie, for a glibc no
# newer than the oldest rootfs's (2.34 on Prime GO 2.4.0). Drivers: virgl for a GL host,
# swrast (softpipe, without LLVM) for one without, which is what kms_swrast_dri.so
# serves.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

FORCE=0
EXTRA_DRIVERS=""
PLATFORMS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --extra-drivers) EXTRA_DRIVERS="$2"; shift 2 ;;
        --platforms) PLATFORMS="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

# For MALI_MESA_VERSION; detect_mesa itself is not run.
# shellcheck source=detect_mesa.sh
. "$REPO_ROOT/scripts/build_scripts/detect_mesa.sh"
MESA_VER="$MALI_MESA_VERSION"
OUT="$OUT_DIR/mesa-$MESA_VER-armhf-mali"
GALLIUM_DRIVERS=virgl,swrast
if [ -n "$EXTRA_DRIVERS" ]; then
    GALLIUM_DRIVERS="$GALLIUM_DRIVERS,$EXTRA_DRIVERS"
    OUT="$OUT+${EXTRA_DRIVERS//,/+}"
fi
case "$PLATFORMS" in
    "") ;;
    wayland) OUT="$OUT+wayland" ;;
    *) echo "ERROR: --platforms takes only wayland." >&2; exit 1 ;;
esac

if [ -s "$OUT/libmali.so.14.0" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    exit 0
fi

# Cross-compiled, so the compiler runs native: arm64 on Apple silicon, amd64
# elsewhere. Pulled explicitly because `docker run --platform` reuses whatever
# architecture a tag was last pulled for.
case "$(uname -m)" in
    arm64|aarch64) PLATFORM=linux/arm64 ;;
    *)             PLATFORM=linux/amd64 ;;
esac
echo "--- building Mesa $MESA_VER (in place of libmali) for armhf ---"
docker pull -q --platform "$PLATFORM" debian:bookworm >/dev/null

STAGE="$(mktemp -d /tmp/build-mali-mesa.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

docker run --rm --platform "$PLATFORM" -e MESA_VER="$MESA_VER" -e GALLIUM_DRIVERS="$GALLIUM_DRIVERS" -e PLATFORMS="$PLATFORMS" -v "$STAGE:/out" \
    debian:bookworm bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        dpkg --add-architecture armhf
        apt-get update -qq
        # build-essential as well as the cross toolchain: Mesa builds and runs
        # generators on the builder.
        apt-get install -y -qq --no-install-recommends \
            build-essential crossbuild-essential-armhf \
            meson ninja-build pkg-config python3 python3-mako python3-yaml \
            flex bison xz-utils ca-certificates curl \
            libdrm-dev:armhf libexpat1-dev:armhf zlib1g-dev:armhf >/dev/null
        # wayland-scanner runs on the builder, the library on the target.
        [ -z "$PLATFORMS" ] || apt-get install -y -qq --no-install-recommends \
            libwayland-bin wayland-protocols libwayland-dev:armhf \
            libwayland-egl-backend-dev:armhf >/dev/null

        curl -fsSL "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz" -o /mesa.tar.xz
        mkdir /src && tar xf /mesa.tar.xz -C /src --strip-components=1
        cd /src

        # pkgconfig, not pkg-config: bookworm has meson 1.0, which predates the
        # second spelling and otherwise finds no pkg-config for the target at all.
        # Double quotes substituted afterwards, as in build_virgl_mesa.sh.
        cat > /cross.txt <<EOF
[binaries]
c = "arm-linux-gnueabihf-gcc"
cpp = "arm-linux-gnueabihf-g++"
ar = "arm-linux-gnueabihf-ar"
strip = "arm-linux-gnueabihf-strip"
pkgconfig = "pkg-config"
[host_machine]
system = "linux"
cpu_family = "arm"
cpu = "armv7"
endian = "little"
EOF
        sed -i "s/\"/\x27/g" /cross.txt
        export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
        export PKG_CONFIG_SYSROOT_DIR=/

        # --prefix /usr --libdir lib, because the paths are compiled in: libgbm
        # and libEGL look for drivers in /usr/lib/dri. No platforms unless asked:
        # drm (gbm), surfaceless and device are always built, and eglfs_kms needs
        # only gbm.
        # gles1 because the blob provided it, though nothing in the rootfs links it.
        #
        # libstdc++ is linked in statically. The GCC 12 headers on bookworm make the
        # driver import std::__throw_bad_array_new_length@GLIBCXX_3.4.29, and Prime
        # GO 2.4.0 ships the libstdc++ of GCC 10, which stops at 3.4.28, so the driver
        # would not load there. The C++ in Mesa is its own (the GLSL compiler) and
        # the driver exports only its entry points, so a private copy never meets
        # the one the app loads.
        meson setup build --cross-file /cross.txt --prefix=/usr --libdir=lib \
            -Dcpp_link_args=-static-libstdc++ \
            -Dbuildtype=release \
            -Dgallium-drivers="$GALLIUM_DRIVERS" \
            -Dvulkan-drivers= \
            -Dplatforms="$PLATFORMS" \
            -Dglx=disabled \
            -Dglvnd=false \
            -Degl=enabled \
            -Dgbm=enabled \
            -Dopengl=true \
            -Dgles1=enabled \
            -Dgles2=enabled \
            -Dllvm=disabled \
            -Dshared-llvm=disabled \
            -Dvideo-codecs= \
            -Dgallium-vdpau=disabled \
            -Dgallium-va=disabled \
            -Dgallium-xa=disabled \
            -Dlmsensors=disabled \
            -Dzstd=disabled \
            -Dbuild-tests=false \
            -Dvalgrind=disabled \
            -Dlibunwind=disabled > /out/meson.log 2>&1 || {
                echo "ERROR: meson setup failed" >&2; tail -30 /out/meson.log >&2; exit 1; }
        ninja -C build > /out/ninja.log 2>&1 || {
            echo "ERROR: mesa build failed" >&2; tail -30 /out/ninja.log >&2; exit 1; }
        DESTDIR=/out/root ninja -C build install > /out/install.log 2>&1

        L=/out/root/usr/lib
        # --no-as-needed, or the linker drops all three for supplying nothing to an
        # empty object, and the stand-in would load no GL at all.
        : > /stand-in.c
        arm-linux-gnueabihf-gcc -shared -o "$L/libmali.so.14.0" /stand-in.c \
            -Wl,-soname,libmali.so.14.0 -Wl,--no-as-needed \
            -L"$L" -lEGL -lGLESv2 -lgbm

        mkdir -p /out/mali
        cp -a "$L"/lib*.so.* /out/mali/
        mkdir /out/mali/dri
        cp -a "$L"/dri/*_dri.so /out/mali/dri/
        # The rootfs has neither libwayland nor, before 4.3, libffi; these are the
        # ones this libEGL was built against.
        if [ -n "$PLATFORMS" ]; then
            mkdir /out/mali/wayland
            T=/usr/lib/arm-linux-gnueabihf
            cp -L $T/libwayland-client.so.0 $T/libwayland-server.so.0 $T/libwayland-cursor.so.0 \
                $T/libwayland-egl.so.1 $T/libffi.so.8 /out/mali/wayland/
        fi
    '

M="$STAGE/mali"
READELF="$(command -v readelf || echo /opt/homebrew/opt/binutils/bin/readelf)"
[ -x "$READELF" ] || { echo "ERROR: readelf not found." >&2; exit 1; }

# The failures here are silent in a guest, so they are checked here. Whole-input
# greps, not grep -q, for the pipefail reason build_virgl_mesa.sh gives.
for f in libEGL.so.1 libGLESv2.so.2 libgbm.so.1 libglapi.so.0 dri/virtio_gpu_dri.so \
         dri/kms_swrast_dri.so libmali.so.14.0; do
    [ -e "$M/$f" ] || { echo "ERROR: the build produced no $f." >&2; exit 1; }
done
_needed="$("$READELF" -d "$M/libmali.so.14.0" | grep NEEDED)"
for _so in libEGL.so.1 libGLESv2.so.2 libgbm.so.1; do
    case "$_needed" in *"[$_so]"*) ;; *)
        echo "ERROR: the stand-in libmali does not pull in $_so." >&2; exit 1 ;;
    esac
done
nm -D --defined-only "$M/dri/virtio_gpu_dri.so" 2>/dev/null |
    grep '__driDriverGetExtensions_virtio_gpu' >/dev/null || {
        echo "ERROR: virtio_gpu_dri.so has no virtio_gpu entry point." >&2; exit 1; }
# Anything newer than the oldest rootfs's glibc fails to load: Prime GO 2.4.0's
# 2.34. libstdc++ is linked in, so nothing may import from it at all.
_syms="$(for f in "$M"/*.so.* "$M"/dri/virtio_gpu_dri.so; do
        "$READELF" --dyn-syms -W "$f"; done)"
if printf '%s\n' "$_syms" | grep 'GLIBCXX_' >/dev/null; then
    echo "ERROR: the build imports from libstdc++, which should be linked in." >&2
    exit 1
fi
_newest="$(printf '%s\n' "$_syms" | grep -oE 'GLIBC_[0-9][0-9.]*' |
    sed 's/^GLIBC_//' | sort -uV | awk 'END {print}')"
[ "$(printf '%s\n%s\n' "$_newest" 2.34 | sort -V | awk 'END {print}')" = 2.34 ] || {
    echo "ERROR: needs GLIBC_$_newest, newer than Prime GO 2.4.0's 2.34." >&2
    exit 1; }

rm -rf "$OUT" && mv "$M" "$OUT"
echo ""
echo "Built: $OUT ($(du -sh "$OUT" | cut -f1))"
