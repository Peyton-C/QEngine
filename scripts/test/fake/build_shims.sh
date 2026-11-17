#!/bin/bash
# build_shims.sh : build the preloaded shims from the working tree, for check.sh -s
# to run in place of the ones an instance was built with.
#
# This is what lets a shim edit be tried against every instance without
# rebuilding any of them. Output goes to $OUT/fake/shims/:
#
#   arm64-rk3588/  arm64-rk3566/  armhf-rk3288/   for guests with glibc 2.34 or later
#   guest/NAME/                                   linked against instance NAME's own libc
#
# The second kind is for firmware older than glibc 2.34, legacy Engine and MPC
# 2.x, whose loader refuses anything linked on bookworm. check.sh fetches those
# libraries from the instance into $OUT/fake/guestlib/NAME/ before calling this.
#
# The compiler lines are copies of the rootfs builders', and have to be kept in
# step with them by hand: build_arm64_rootfs.sh, build_armv7_engine_rootfs.sh
# and build_mpc_rootfs.sh for the first kind, build_legacy_engine_shims.sh and
# build_mpc_rootfs.sh's LEGACY_LIBC branch for the second. A shim that builds
# here and not there, or the other way round, means they have drifted.
# qmlrotate, MONTERO, maliwindow and qt55vnc need Qt or GL to build and are left
# as the instance has them.
set -eu
source "$(dirname "$0")/../common.sh"
out="$OUT/fake/shims"; libs="$OUT/fake/guestlib"
mkdir -p "$out" "$libs"

# Nothing to do when the last build is newer than every source and library.
if [ -f "$out/.built" ] &&
        [ -z "$(find "$ROOT/shims" "$libs" "$0" -newer "$out/.built" -type f | head -n 1)" ]; then
    exit 0
fi

build() {
    local platform=$1 want=$2
    { echo "--- building shims from the working tree for $platform ---" >/dev/tty; } 2>/dev/null || true
    docker pull -q --platform "$platform" debian:bookworm >/dev/null
    docker run --rm --platform "$platform" -e WANT="$want" \
        -v "$ROOT/shims:/shims:ro" -v "$libs:/guestlib:ro" -v "$out:/out" \
        debian:bookworm bash -c '
        set -e
        # Not uname -m: an armhf container on an arm64 kernel runs natively,
        # as on the arm64 CI runners, and reports aarch64 there.
        arch=$(dpkg --print-architecture)
        [ "$arch" = "$WANT" ] || {
            echo "ERROR: shim container is $arch, expected $WANT." >&2; exit 1; }
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc libc6-dev libdrm-dev libasound2-dev >/dev/null 2>&1

        if [ $arch = arm64 ]; then socs="RK3588 RK3566"; else socs=RK3288; fi
        for soc in $socs; do
            o=/out/$arch-$(echo $soc | tr A-Z a-z); mkdir -p $o
            gcc -shared -fPIC -O2 -Wall -o $o/dtshim.so /shims/dtshim/dtshim.c -DSOC_$soc -ldl -lpthread
            gcc -shared -fPIC -O2 -I/usr/include/libdrm -o $o/drmatomic.so /shims/drmatomic/drmatomic.c -ldl
            gcc -shared -fPIC -O2 -Wall -o $o/alsashim.so /shims/alsashim/alsashim.c -ldl -lpthread
            gcc -shared -fPIC -O2 -Wall -o $o/quitshim.so /shims/quitshim/quitshim.c -ldl -lpthread
            gcc -shared -fPIC -O2 -Wall -o $o/mpcsurface.so /shims/mpcsurface/mpcsurface.c -ldl -lpthread
            gcc -O2 -Wall -o $o/midisurface /shims/midisurface/midisurface.c -lasound
            if [ $arch = arm64 ]; then
                gcc -shared -fPIC -O2 -Wall -o $o/teeshim.so /shims/teeshim/teeshim.c
            fi
        done
        [ $arch = armhf ] || exit 0

        # Bookworm compiles, the guest libraries link, and -z defs makes a
        # symbol the guest lacks a build error.
        for G in /guestlib/*/; do
            [ -f "$G/libc.so" ] || continue
            name=$(basename "$G"); o=/out/guest/$name; mkdir -p $o
            device=$(sed -n "s/^DEVICE=//p" "$G/instance.env")
            rt=; [ ! -f "$G/librt.so" ] || rt="$G/librt.so"
            gcc -fPIC -O2 -Wall -DSOC_RK3288 -c /shims/dtshim/dtshim.c -o /tmp/dtshim.o
            gcc -fPIC -O2 -Wall -c /shims/alsashim/alsashim.c -o /tmp/alsashim.o
            if [ "${device:-engine}" = engine ]; then
                gcc -fPIC -O2 -Wall -c /shims/dtshim/legacy_compat.c -o /tmp/compat.o
                gcc -shared -nostdlib -Wl,-z,defs -o $o/dtshim.so /tmp/dtshim.o /tmp/compat.o -lgcc \
                    $G/libc.so $G/libdl.so $G/libpthread.so $G/ld.so
                gcc -shared -nostdlib -Wl,-z,defs -o $o/alsashim.so /tmp/alsashim.o \
                    $G/libc.so $G/libdl.so $G/libpthread.so $rt -lgcc
            else
                gcc -fPIC -O2 -Wall -c /shims/mpcsurface/mpcsurface.c -o /tmp/mpcsurface.o
                gcc -fPIC -O2 -Wall -c /shims/mpcsurface/legacy_compat.c -o /tmp/mpcsurface_compat.o
                for s in dtshim mpcsurface alsashim; do
                    objs=/tmp/$s.o; [ $s != mpcsurface ] || objs="$objs /tmp/mpcsurface_compat.o"
                    gcc -shared -nostdlib -Wl,-z,defs -o $o/$s.so $objs -lgcc \
                        $G/libc.so $G/libdl.so $G/libpthread.so $rt $G/ld.so
                done
            fi
        done
    '
}
# The builders' -Wall output is long and the same every time, so it is kept in
# a log and shown only when a build fails.
for target in 'linux/arm64 arm64' 'linux/arm/v7 armhf'; do
    # shellcheck disable=SC2086  # two words: the platform and the Debian architecture
    build $target > "$out/build.log" 2>&1 || { cat "$out/build.log" >&2; exit 1; }
done
touch "$out/.built"
