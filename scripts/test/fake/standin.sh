#!/bin/bash
# standin.sh [-b] [NAME ...] : run the fake Engine with no firmware at all, in
# stock distribution containers standing in for each firmware era.
#
#   -b         save the results as the tracked baseline, fake/standin/NAME.tap
#   NAME ...   stand-ins from the table below; with none, all of them
#
# This is the part of check.sh that needs nothing of inMusic's, so it can run
# anywhere Docker does, a GitHub runner included. Each stand-in is a container
# whose glibc matches an era's, given the working tree's shims, the fixtures
# write_fake_dt.sh writes and the LD_PRELOAD list that era's builder sets.
#
# What that leaves out is everything a real rootfs brings. There is no real
# application to read imports from, so every libc entry point counts as one the
# application might use. There is no sound card or sequencer, and the IRQs are
# the runner's, so only the probes that depend on none of that run: loader,
# identity, devmem, devicetree, efuse, battery, tee and quit. And a distribution's
# glibc is only close to the firmware's, 2.24 for SC5000's 2.22 to 2.25. A
# shim that passes here can still be refused by a real guest, which is what
# check.sh against instances is for.
#
# The baseline is tracked, unlike check.sh's, because nothing in it comes from
# firmware and a runner starts with an empty build/. It holds statuses only.
set -eu
source "$(dirname "$0")/../common.sh"
FAKE="$TEST_SCRIPTS/fake"
PROBES=loader,identity,devmem,devicetree,efuse,battery,tee,quit

# name | platform | image | DEVICE | soc | product code | shims | LD_PRELOAD
#
# The images are picked for their glibc: Ubuntu 24.04 has 2.39 and, on armhf, a
# 64-bit time_t, as Yocto-built Engine 5.x does. Bookworm has 2.36 with a 32-bit
# one, as 4.3.4 does. Stretch and buster, 2.24 and 2.28, are older than 2.34,
# so their shims are linked against the container's own libc, the way the
# legacy builders link against the guest's. JP11 is there for its battery. The
# arm64 MPC, MPC Sample and VX5 images' glibc was not checked: 2.39 is a guess.
STANDINS="\
modern-arm64|linux/arm64|ubuntu:24.04|engine|rk3588|JP24|arm64-rk3588|dtshim drmatomic alsashim teeshim quitshim
modern-armv7|linux/arm/v7|ubuntu:24.04|engine|rk3288|JC11|armhf-rk3288|dtshim drmatomic alsashim quitshim
classic-armv7|linux/arm/v7|debian:bookworm|engine|rk3288|JP11|armhf-rk3288|dtshim drmatomic alsashim quitshim
legacy-armv7|linux/arm/v7|debian/eol:stretch|engine|rk3288|JP07|guest|dtshim alsashim
mpc-armv7|linux/arm/v7|debian:bookworm|mpc|rk3288|ACV5|armhf-rk3288|dtshim mpcsurface alsashim
mpc-arm64|linux/arm64|ubuntu:24.04|mpc|rk3588|ACVA3|arm64-rk3588|dtshim mpcsurface alsashim
sample-arm64|linux/arm64|ubuntu:24.04|mpc-sample|rk3566|AC50|arm64-rk3566|dtshim mpcsurface alsashim
vx5-arm64|linux/arm64|ubuntu:24.04|headrush|rk3566|HV03|arm64-rk3566|dtshim alsashim
mpc2-armv7|linux/arm/v7|debian/eol:buster|mpc|rk3288|ACV5|guest|dtshim mpcsurface alsashim"

save=
if [ "${1:-}" = -b ]; then save=--save; shift; fi
# shellcheck disable=SC2046  # one word per stand-in is the point
[ $# -gt 0 ] || set -- $(printf '%s\n' "$STANDINS" | cut -d'|' -f1)
row() { printf '%s\n' "$STANDINS" | grep "^$1|" || { echo "standin.sh: no stand-in $1" >&2; exit 2; }; }

# The old-glibc stand-ins' own libc, where build_shims.sh looks for an
# instance's. Fetched once: an image's libc does not change.
for name in "$@"; do
    IFS='|' read -r _ platform image device _ _ shims _ <<< "$(row "$name")"
    dest="$OUT/fake/guestlib/standin-$name"
    [ "$shims" = guest ] && [ ! -f "$dest/libc.so" ] || continue
    mkdir -p "$dest"
    docker pull -q --platform "$platform" "$image" >/dev/null
    docker run --rm --platform "$platform" -v "$dest:/dest" "$image" sh -c '
        for lib in libc libdl libpthread librt ld; do
            cp /lib/arm-linux-gnueabihf/$lib-2.*.so /dest/$lib.so
        done'
    echo "DEVICE=$device" > "$dest/instance.env"
done
"$FAKE/build.sh"
"$FAKE/build_shims.sh"

results="$OUT/fake/results/standin"
mkdir -p "$results"
for name in "$@"; do
    IFS='|' read -r _ platform image device soc code shims preload <<< "$(row "$name")"
    [ "$shims" != guest ] || shims="guest/standin-$name"
    case "$platform" in linux/arm64) arch=arm64 ;; *) arch=armhf ;; esac
    # Naming probes overrides the fake's own choice by application, so the
    # choice is made here, as main.c makes it. Only the Engine builders pass
    # write_fake_dt a SoC, which is what writes the eFuse and battery fixtures.
    probes=$PROBES; fixtures_soc=$soc
    if [ "$device" != engine ]; then
        probes=loader,identity,devmem; fixtures_soc=
        [ "$soc" != rk3566 ] || probes=$probes,devicetree
    fi
    echo "--- $name ---" >&2
    docker pull -q --platform "$platform" "$image" >/dev/null
    # Privileged for nothing but parity with a guest, where Engine is root.
    docker run --rm --privileged --platform "$platform" \
        -e CODE="$code" -e SOC="$soc" -e FIXTURES_SOC="$fixtures_soc" -e DEVICE="$device" -e PRELOAD="$preload" -e PROBES="$probes" \
        -v "$OUT/fake/bin/$arch:/fake:ro" -v "$OUT/fake/shims/$shims:/built:ro" \
        -v "$ROOT/scripts/build_scripts/rootfs_steps:/steps:ro" "$image" bash -c '
        set -e
        export DEBIAN_FRONTEND=noninteractive
        # The fake links libasound, as Engine does. Ubuntu 24.04 renamed the
        # package and only sometimes answers to the old name.
        apt_install() { apt-get install -y -qq --no-install-recommends \
            -o APT::Get::AllowUnauthenticated=true "$1"; }
        { apt-get update -qq && { apt_install libasound2t64 || apt_install libasound2; }
        } >/tmp/apt.log 2>&1 || { cat /tmp/apt.log >&2; exit 1; }
        . /steps/write_fake_dt.sh
        write_fake_dt "" "$CODE" "" "$FIXTURES_SOC"
        mkdir -p /run/qengine-fake
        cp /fake/Engine /run/qengine-fake/Engine
        list=
        for shim in $PRELOAD; do
            cp "/built/$shim.so" "/root/$shim.so"
            list="$list${list:+:}/root/$shim.so"
        done
        echo "# stand-in: $(ldd --version | head -n 1), $(dpkg --print-architecture 2>/dev/null || uname -m)"
        LD_PRELOAD="$list" /run/qengine-fake/Engine --app "$DEVICE" --soc "$SOC" \
            --code "$CODE" --probes "$PROBES"
    ' > "$results/$name.tap" 2> "$results/$name.log" || true
done

if [ -n "$save" ]; then
    # Statuses alone: details such as import counts would only add churn.
    mkdir -p "$FAKE/standin"
    for name in "$@"; do
        sed -nE 's/^((not )?ok [^ ]+)( # SKIP)?.*/\1\3/p; /^1\.\./p' "$results/$name.tap" \
            > "$FAKE/standin/$name.tap"
    done
fi
python3 "$FAKE/report.py" "$results" "$FAKE/standin" "$@"
