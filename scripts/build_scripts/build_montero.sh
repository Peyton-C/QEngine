#!/bin/bash
# build_montero.sh — build MONTERO, the QML mod loader, and every mod under
# shims/montero/mods, for one guest architecture. Called by the Engine rootfs
# builders when MONTERO=1; rootfs_steps/install_montero.sh installs the result.
#
# Usage: build_montero.sh --arch <arm64|armhf> --stage <dir>
#   --arch   the guest architecture to build for
#   --stage  the builder's staging directory. The mods are written under
#            <stage>/montero-mods/<mod>/*.rcc, as MONTERO looks for them. They
#            are build output with no per-architecture name, so they go here
#            rather than into shims/.
#
# Writes shims/montero/montero_<arch>.so, named like the other shared shims.
#
# A container of its own because MONTERO is C++ against Qt rather than C against
# libc: the Qt headers and tools cost more to install than every other shim
# together, so a build that does not want it does not pay for them.
#
# Built against bookworm's Qt 6.4, older than any Qt 6 Engine OS ships (5.3.0 has
# 6.7.2). Qt 6 keeps binary compatibility across minor releases in that
# direction, as glibc does. An .rcc is a plain resource bundle read at runtime,
# so the Qt version that built it does not matter.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH=""
STAGE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --stage) STAGE="$2"; shift 2 ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

case "$ARCH" in
    arm64) PLATFORM=linux/arm64;  EXPECT='aarch64|arm64' ;;
    armhf) PLATFORM=linux/arm/v7; EXPECT='armv7l|armv8l' ;;
    *) echo "ERROR: --arch must be arm64 or armhf." >&2; exit 1 ;;
esac
[ -d "$STAGE" ] || { echo "ERROR: --stage must be an existing directory." >&2; exit 1; }

echo "--- building MONTERO and its mods for $ARCH ---"
docker pull -q --platform "$PLATFORM" debian:bookworm >/dev/null
docker run --rm --platform "$PLATFORM" \
    -e SHIM_ARCH="$ARCH" \
    -e EXPECT="$EXPECT" \
    -v "$REPO_ROOT/shims:/shims" \
    -v "$STAGE:/stage" \
    debian:bookworm bash -c '
        set -e
        # Copied straight into the guest rootfs, so a wrong-architecture
        # container would graft foreign binaries in. Fail loudly instead.
        if ! printf "%s" "$(uname -m)" | grep -qxE "$EXPECT"; then
            echo "ERROR: MONTERO container is $(uname -m), expected $EXPECT." >&2; exit 1
        fi
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq g++ pkg-config qt6-base-dev qt6-declarative-dev >/dev/null 2>&1
        QT_LIBEXEC="$(pkg-config --variable=libexecdir Qt6Core)"
        QT_MODULES="Qt6Core Qt6Qml Qt6Quick"

        # montero.cpp includes its own moc output, for the Montero singleton.
        mkdir -p /tmp/moc
        "$QT_LIBEXEC/moc" /shims/montero/montero.cpp -o /tmp/moc/montero.moc
        g++ -shared -fPIC -O2 -Wall -std=c++17 -I/tmp/moc $(pkg-config --cflags $QT_MODULES) \
            -o /shims/montero/montero_$SHIM_ARCH.so /shims/montero/montero.cpp \
            $(pkg-config --libs $QT_MODULES) -ldl

        rm -rf /stage/montero-mods
        mkdir -p /stage/montero-mods
        for qrc in /shims/montero/mods/*/*.qrc; do
            [ -e "$qrc" ] || continue
            mod="$(basename "$(dirname "$qrc")")"
            mkdir -p "/stage/montero-mods/$mod"
            "$QT_LIBEXEC/rcc" -binary "$qrc" -o "/stage/montero-mods/$mod/$(basename "$qrc" .qrc).rcc"
            echo "    mod: $mod/$(basename "$qrc" .qrc).rcc"
        done
    '

[ -s "$REPO_ROOT/shims/montero/montero_$ARCH.so" ] || {
    echo "ERROR: MONTERO build produced no montero/montero_$ARCH.so" >&2; exit 1; }
