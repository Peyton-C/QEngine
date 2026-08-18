#!/bin/bash
# build_virgl_dri.sh — builds the Mesa DRI driver that lets a guest render through
# virgl on the host's GPU, instead of rasterizing every frame on an emulated CPU.
#
# Usage: build_virgl_dri.sh --arch <arm64|armhf> [--force]
#   Writes virtio_gpu_dri-<arch>.so to the repo's build/ and reuses it on later
#   runs, the same way get_kernel.sh caches a kernel per architecture. Requires
#   Docker.
#
# Why this is built rather than copied out of Debian, which is what the rootfs
# builders used to do:
#
# Debian ships one megadriver containing every gallium driver, and installs it
# under each driver's name. So its virtio_gpu_dri.so needs libLLVM (for llvmpipe
# and radeonsi), libdrm_radeon, libdrm_amdgpu, libdrm_nouveau, libsensors,
# libxcb-dri3 and libelf -- for hardware and code paths virgl never touches. A
# stripped vendor rootfs has none of them, so Mesa's loader dlopens the driver,
# the dlopen fails, and Mesa falls back to swrast *without a word in any log*: the
# kernel still reports "[drm] features: +virgl", Engine still commits a KMS
# modeset, and nothing mentions kms_swrast. It looks exactly like success. On a
# JP13 guest it cost 617ms per frame instead of 18ms, and went unnoticed until
# frame times were measured.
#
# A virgl-only build has none of that: 11MB, and its dependencies are libglapi,
# libdrm, libexpat, libz, libstdc++, libm, libgcc_s and libc, all of which these
# rootfs images already carry.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

# Matched to the Mesa the guests ship, deliberately. The DRI driver is loaded by
# the guest's own libEGL/libgbm, so the two have to agree on the driver ABI, and
# the file that used to be copied in was Mesa 22.3.6 against a 24.0.7 loader.
# Check a guest with: strings /usr/lib/dri/kms_swrast_dri.so | grep -o 'Mesa [0-9.]*'
MESA_VER=24.0.7

ARCH=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

case "$ARCH" in
    arm64) TRIPLE=aarch64-linux-gnu; CPU_FAMILY=aarch64; CPU=aarch64 ;;
    armhf) TRIPLE=arm-linux-gnueabihf; CPU_FAMILY=arm; CPU=armv7 ;;
    *) echo "ERROR: --arch must be arm64 or armhf." >&2; exit 1 ;;
esac

OUT="$OUT_DIR/virtio_gpu_dri-$ARCH.so"
if [ -s "$OUT" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    exit 0
fi

# Cross-compiled on the host architecture rather than built in an emulated
# container: the compiler runs native and only its output is ARM, which is minutes
# instead of hours. `docker run --platform` does not re-pull, so a tag already
# cached for another architecture would be reused silently -- hence the explicit
# pull, and the assertion inside.
echo "--- building a virgl-only Mesa $MESA_VER DRI driver for $ARCH ---"
docker pull -q --platform linux/amd64 debian:trixie >/dev/null

STAGE="$(mktemp -d /tmp/build-virgl-dri.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

docker run --rm --platform linux/amd64 \
    -e TRIPLE="$TRIPLE" -e DEBARCH="$ARCH" -e MESA_VER="$MESA_VER" \
    -e CPU_FAMILY="$CPU_FAMILY" -e CPU="$CPU" \
    -v "$STAGE:/out" \
    debian:trixie bash -euo pipefail -c '
        case "$(uname -m)" in x86_64|amd64) ;; *)
            echo "ERROR: builder is $(uname -m); a cross build needs a native host." >&2
            exit 1 ;;
        esac
        export DEBIAN_FRONTEND=noninteractive
        dpkg --add-architecture "$DEBARCH"
        apt-get update -qq
        # build-essential is not redundant with the cross toolchain: Mesa generates
        # and runs tools during the build, and those must run on the builder.
        apt-get install -y -qq --no-install-recommends \
            build-essential crossbuild-essential-$DEBARCH \
            meson ninja-build pkg-config python3 python3-mako python3-yaml \
            flex bison xz-utils ca-certificates curl \
            libdrm-dev:$DEBARCH libexpat1-dev:$DEBARCH zlib1g-dev:$DEBARCH >/dev/null

        curl -fsSL "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz" -o /mesa.tar.xz
        mkdir /src && tar xf /mesa.tar.xz -C /src --strip-components=1
        cd /src

        # Meson machine files reject double-quoted values, and this script cannot
        # contain single quotes (it is itself single-quoted), so the quotes are
        # substituted afterwards.
        cat > /cross.txt <<EOF
[binaries]
c = "$TRIPLE-gcc"
cpp = "$TRIPLE-g++"
ar = "$TRIPLE-ar"
strip = "$TRIPLE-strip"
pkg-config = "pkg-config"
[host_machine]
system = "linux"
cpu_family = "$CPU_FAMILY"
cpu = "$CPU"
endian = "little"
EOF
        sed -i "s/\"/\x27/g" /cross.txt
        export PKG_CONFIG_LIBDIR="/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig"
        export PKG_CONFIG_SYSROOT_DIR=/

        # virgl alone, and llvm off: llvm is what llvmpipe and radeonsi need, and
        # dragging it in is the whole problem this script exists to avoid.
        #
        # egl and gbm stay enabled even though the guest supplies its own. In Mesa
        # 24 the gallium DRI target is only built when a DRI-based API asks for it,
        # so disabling them to keep the build small produces a tree with no
        # *_dri.so in it at all. Their other outputs are simply not copied out.
        meson setup build --cross-file /cross.txt \
            -Dbuildtype=release \
            -Dgallium-drivers=virgl \
            -Dvulkan-drivers= \
            -Dplatforms= \
            -Dglx=disabled \
            -Degl=enabled \
            -Dgbm=enabled \
            -Dopengl=true \
            -Dgles1=disabled \
            -Dgles2=enabled \
            -Dllvm=disabled \
            -Dshared-llvm=disabled \
            -Dvideo-codecs= \
            -Dgallium-vdpau=disabled \
            -Dgallium-va=disabled \
            -Dlmsensors=disabled \
            -Dbuild-tests=false \
            -Dvalgrind=disabled \
            -Dlibunwind=disabled > /out/meson.log 2>&1 || {
                echo "ERROR: meson setup failed" >&2; tail -30 /out/meson.log >&2; exit 1; }

        ninja -C build > /out/ninja.log 2>&1 || {
            echo "ERROR: mesa build failed" >&2; tail -30 /out/ninja.log >&2; exit 1; }

        D=$(find build -name virtio_gpu_dri.so | head -n 1)
        [ -n "$D" ] || { echo "ERROR: no virtio_gpu_dri.so was produced." >&2; exit 1; }
        cp "$D" /out/virtio_gpu_dri.so
    '

[ -s "$STAGE/virtio_gpu_dri.so" ] || {
    echo "ERROR: the build produced no driver." >&2; exit 1; }

# Assert the two properties that actually matter, because the failure mode this
# script exists to prevent is silent. A driver missing its loader entry point, or
# needing a library the guest lacks, fails at dlopen and Mesa quietly uses swrast.
if ! nm -D --defined-only "$STAGE/virtio_gpu_dri.so" 2>/dev/null |
        grep -q '__driDriverGetExtensions_virtio_gpu'; then
    echo "ERROR: the driver does not export __driDriverGetExtensions_virtio_gpu," >&2
    echo "       so the guest's Mesa loader would never use it." >&2
    exit 1
fi
if readelf -d "$STAGE/virtio_gpu_dri.so" | grep NEEDED |
        grep -qE 'libLLVM|libdrm_(radeon|amdgpu|nouveau)|libsensors|libxcb|libelf'; then
    echo "ERROR: the driver still needs libraries a vendor rootfs does not have:" >&2
    readelf -d "$STAGE/virtio_gpu_dri.so" | grep NEEDED | sed 's/^/       /' >&2
    exit 1
fi

mv "$STAGE/virtio_gpu_dri.so" "$OUT"
echo ""
echo "Built: $OUT ($(du -h "$OUT" | cut -f1))"
readelf -d "$OUT" | grep NEEDED | grep -oE '\[[^]]+\]' | tr -d '[]' |
    tr '\n' ' ' | sed 's/^/  needs: /'
echo ""
