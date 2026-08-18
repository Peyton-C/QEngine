#!/bin/bash
# build_virgl_mesa.sh — builds the Mesa component that lets a guest render through
# virgl on the host's GPU, instead of rasterizing every frame on an emulated CPU.
#
# Usage: build_virgl_mesa.sh --arch <arm64|armhf> [--force]
#   Caches its output in the repo's build/ and reuses it on later runs, the way
#   get_kernel.sh caches a kernel per architecture. Requires Docker.
#
# What it builds differs by guest, because the two vendor Mesa builds load drivers
# by different mechanisms -- established by looking at what Engine actually maps:
#
#   armhf   a DRI driver, virtio_gpu_dri.so. That guest's Mesa dlopens
#           /usr/lib/dri/<name>_dri.so, and its own build has no virgl in it.
#
#   arm64   a whole replacement libgallium-<ver>.so. That guest's Mesa is a
#           shared-gallium build: every driver is compiled inside that one library
#           and libEGL calls into it directly through versioned symbols. There is
#           no plug-in slot, so a DRI driver dropped into /usr/lib/dri is never
#           opened -- which is what the arm64 builder used to do, to no effect.
#
# The Mesa version is pinned per guest and they differ. For the DRI driver it has
# to match because the guest's libEGL loads it across the DRI ABI; for the shared
# library it has to match exactly, because the symbol version node is named after
# the file itself.
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
# JP13 guest that was the difference between a slideshow and a usable UI, and it
# went unnoticed until frame times were measured -- every other signal said the
# driver was working.
#
# A virgl-only build has none of that: 11MB, and its dependencies are libglapi,
# libdrm, libexpat, libz, libstdc++, libm, libgcc_s and libc, all of which these
# rootfs images already carry.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

ARCH=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

# Per-guest configuration. Read a guest's own version rather than trusting this
# table if a firmware update might have moved it:
#   armhf:  strings /usr/lib/dri/kms_swrast_dri.so | grep -o 'Mesa [0-9.]*'
#   arm64:  ls /usr/lib/libgallium-*.so
case "$ARCH" in
    arm64)
        TRIPLE=aarch64-linux-gnu; CPU_FAMILY=aarch64; CPU=aarch64
        MESA_VER=24.3.4
        MODE=gallium
        # softpipe alongside virgl so a guest with no GL host still has the
        # software path the vendor library provided. The hardware drivers the
        # vendor also built -- panfrost, lima, etnaviv, zink -- are deliberately
        # left out: they drive silicon no emulated guest has, and zink would pull
        # in a Vulkan stack. That makes this library wrong for real hardware.
        GALLIUM_DRIVERS=virgl,softpipe
        ARTIFACT="libgallium-$MESA_VER.so"
        OUT="$OUT_DIR/libgallium-$MESA_VER-$ARCH.so"
        ;;
    armhf)
        TRIPLE=arm-linux-gnueabihf; CPU_FAMILY=arm; CPU=armv7
        MESA_VER=24.0.7
        MODE=dri
        GALLIUM_DRIVERS=virgl
        ARTIFACT=virtio_gpu_dri.so
        OUT="$OUT_DIR/virtio_gpu_dri-$ARCH.so"
        ;;
    *) echo "ERROR: --arch must be arm64 or armhf." >&2; exit 1 ;;
esac
if [ -s "$OUT" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- $(basename "$OUT") exists, keeping it (pass --force to rebuild) ---"
    exit 0
fi

# Cross-compiled on the host architecture rather than built in an emulated
# container: the compiler runs native and only its output is ARM, which is minutes
# instead of hours. `docker run --platform` does not re-pull, so a tag already
# cached for another architecture would be reused silently -- hence the explicit
# pull, and the assertion inside.
echo "--- building Mesa $MESA_VER ($MODE, drivers: $GALLIUM_DRIVERS) for $ARCH ---"
docker pull -q --platform linux/amd64 debian:trixie >/dev/null

STAGE="$(mktemp -d /tmp/build-virgl-dri.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

docker run --rm --platform linux/amd64 \
    -e TRIPLE="$TRIPLE" -e DEBARCH="$ARCH" -e MESA_VER="$MESA_VER" \
    -e CPU_FAMILY="$CPU_FAMILY" -e CPU="$CPU" \
    -e GALLIUM_DRIVERS="$GALLIUM_DRIVERS" -e ARTIFACT="$ARTIFACT" \
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
            -Dgallium-drivers="$GALLIUM_DRIVERS" \
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

        D=$(find build -name "$ARTIFACT" | head -n 1)
        [ -n "$D" ] || { echo "ERROR: no $ARTIFACT was produced." >&2; find build -name "*.so" | head -n 10 >&2; exit 1; }
        cp "$D" "/out/$ARTIFACT"
    '

[ -s "$STAGE/$ARTIFACT" ] || {
    echo "ERROR: the build produced no $ARTIFACT." >&2; exit 1; }

# Assert what a working result must look like. The failure mode this script exists
# to prevent is silent -- Mesa falls back to software with nothing logged -- so a
# broken artifact is indistinguishable from a working one once it is in a guest.
# grep reads its whole input on purpose here and below. Under `set -o pipefail`
# a `grep -q` exits at the first match, the command feeding it dies of SIGPIPE,
# and the pipeline reports failure -- so a match looks like a miss, and an
# inverted test like the NEEDED one below looks like a pass.
if ! strings -n 4 "$STAGE/$ARTIFACT" | grep -w virgl >/dev/null; then
    echo "ERROR: no virgl in the result; it would render in software." >&2
    exit 1
fi
case "$MODE" in
dri)
    if ! nm -D --defined-only "$STAGE/$ARTIFACT" 2>/dev/null |
            grep '__driDriverGetExtensions_virtio_gpu' >/dev/null; then
        echo "ERROR: no __driDriverGetExtensions_virtio_gpu, so the loader skips it." >&2
        exit 1
    fi
    # Any of these means a failed dlopen in a stripped rootfs, hence silent swrast.
    if readelf -d "$STAGE/$ARTIFACT" | grep NEEDED |
            grep -E 'libLLVM|libdrm_(radeon|amdgpu|nouveau)|libsensors|libxcb|libelf' >/dev/null; then
        echo "ERROR: it needs libraries a vendor rootfs does not have:" >&2
        readelf -d "$STAGE/$ARTIFACT" | grep NEEDED | sed 's/^/       /' >&2
        exit 1
    fi
    ;;
gallium)
    # The guest's libEGL resolves against this file by soname and by a symbol
    # version node named after it, so a mismatch breaks GL outright.
    if ! readelf -d "$STAGE/$ARTIFACT" | grep -i "soname.*$ARTIFACT" >/dev/null; then
        echo "ERROR: soname does not match $ARTIFACT; the guest would not load it." >&2
        exit 1
    fi
    for sym in dri_create_drawable driCreateContextAttribs dri2_from_dma_bufs; do
        nm -D --defined-only "$STAGE/$ARTIFACT" | awk '{print $3}' |
            sed 's/@.*//' | grep -x "$sym" >/dev/null || {
                echo "ERROR: missing $sym, which the guest libEGL imports." >&2
                exit 1; }
    done
    ;;
esac

mv "$STAGE/$ARTIFACT" "$OUT"
echo ""
echo "Built: $OUT ($(du -h "$OUT" | cut -f1))"
readelf -d "$OUT" | grep NEEDED | grep -oE '\[[^]]+\]' | tr -d '[]' |
    tr '\n' ' ' | sed 's/^/  needs: /'
echo ""
