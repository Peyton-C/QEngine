#!/bin/bash
# build.sh [ARCH ...] : build the fake Engine for arm64 and armhf, or those named,
# into $OUT/fake/bin/ARCH/Engine.
#
# Built on Debian jessie, glibc 2.19, so the oldest firmware loads it: SC5000
# 1.0.1 has glibc 2.22, and a binary linked against a newer one than the guest's
# is refused by its loader. Nothing here needs a newer libc, since the program
# looks up the entry points it tests by name. See main.c.
set -eu
source "$(dirname "$0")/../common.sh"
src="$TEST_SCRIPTS/fake"
[ $# -gt 0 ] || set -- arm64 armhf
for arch in "$@"; do
    case "$arch" in
        arm64) platform=linux/arm64 ;;
        armhf) platform=linux/arm/v7 ;;
        *) echo "build.sh: unknown architecture $arch" >&2; exit 2 ;;
    esac
    out="$OUT/fake/bin/$arch"
    mkdir -p "$out"
    # Nothing to do when the binary is newer than every source.
    if [ -x "$out/Engine" ] && [ -z "$(find "$src" -name '*.[ch]' -newer "$out/Engine")" ]; then
        continue
    fi
    echo "--- building the fake Engine for $arch ---" >&2
    # As in the rootfs builders: without the pull, --platform reuses whichever
    # architecture's image is cached.
    docker pull -q --platform "$platform" debian/eol:jessie >/dev/null
    docker run --rm --platform "$platform" -v "$src:/src:ro" -v "$out:/out" \
        debian/eol:jessie bash -c '
            set -e
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq >/dev/null 2>&1
            apt-get install -y -qq --force-yes --no-install-recommends \
                gcc libc6-dev libasound2-dev >/dev/null 2>&1
            # -rdynamic: quitshim finds QCoreApplication::quit with dlsym.
            gcc -std=gnu99 -O1 -g -Wall -Wextra -rdynamic -o /out/Engine /src/*.c \
                -lasound -ldl -lm
        '
done
