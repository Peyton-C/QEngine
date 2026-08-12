#!/bin/bash
# build_az04_audio.sh — builds the experimental az04-codec/az04-card ASoC
# kernel modules (see docs/ENGINEOS.md's "Audio playback: a real ALSA
# card, reached and opened" and docs/BUILDING.md's "Reimplementing
# az04-codec as a loadable kernel module"), plus the three snd-soc-core
# dependency modules this project's trimmed initrd doesn't ship
# (get_arm64_kernel.sh's MODULES= list has no ASoC support at all).
#
# Sources live in shims/rk3588/az04-audio/ (az04_codec.c, az04_card.c,
# Makefile) and are built in place there, matching this project's
# existing shim convention (dtshim/drmatomic/touchbridge_rmz2 are also
# co-located with their source, not written to build/). The three vendor
# dependency modules are not this project's code, so they're kept
# separate, under build/az04-audio/.
#
# Requires an *exact* kernel-version match against whatever
# get_arm64_kernel.sh most recently produced (CONFIG_MODVERSIONS means
# even a matching kernel release with different symbol CRCs won't load)
# — run this script around the same time as get_arm64_kernel.sh, from
# the same apt snapshot, to keep them in sync. Both pull from Debian
# trixie's linux-image-arm64/linux-headers-arm64 packages.
#
# Usage: build_az04_audio.sh
#   Writes az04_codec.ko + az04_card.ko to shims/rk3588/az04-audio/, and
#   snd-soc-core.ko + snd-pcm-dmaengine.ko + snd-compress.ko to
#   build/az04-audio/. Requires Docker.
#
# These are diagnostic/experimental modules, not part of the normal
# boot path — see docs/BUILDING.md for how to deploy them into an
# already-booted guest (mount rw, curl over the host's usermode-network
# gateway at 10.0.2.2, insmod in dependency order: snd-pcm-dmaengine,
# snd-compress, snd-soc-core, az04_codec, az04_card).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_DIR="$REPO_ROOT/shims/rk3588/az04-audio"
OUT_DIR="$REPO_ROOT/build/az04-audio"

command -v docker >/dev/null 2>&1 || { echo "ERROR: 'docker' is required but not found on PATH." >&2; exit 1; }

for f in az04_codec.c az04_card.c Makefile; do
    [ -f "$SRC_DIR/$f" ] || { echo "ERROR: expected source file missing: $SRC_DIR/$f" >&2; exit 1; }
done

mkdir -p "$OUT_DIR"

echo "Building az04-audio kernel modules..."

INNER_SCRIPT="$(mktemp /tmp/build-az04-audio-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq linux-image-arm64 linux-headers-arm64 build-essential >/dev/null 2>&1

KVER=$(ls /lib/modules/)
echo "Kernel version: $KVER"

echo "--- building az04_codec.ko + az04_card.ko ---"
cd /src
make KDIR="/lib/modules/$KVER/build"

echo "--- copying dependency modules (not this project's code) ---"
SOC_DIR="/lib/modules/$KVER/kernel/sound/soc"
CORE_DIR="/lib/modules/$KVER/kernel/sound/core"
for pair in "$SOC_DIR/snd-soc-core.ko.xz:snd-soc-core.ko" \
            "$CORE_DIR/snd-pcm-dmaengine.ko.xz:snd-pcm-dmaengine.ko" \
            "$CORE_DIR/snd-compress.ko.xz:snd-compress.ko"; do
    src="${pair%%:*}"
    name="${pair##*:}"
    if [ ! -f "$src" ]; then
        echo "ERROR: expected vendor module missing: $src (Debian trixie's kernel package layout may have changed)" >&2
        exit 1
    fi
    xz -dc "$src" > "/out/$name"
done

echo "--- cleaning up build intermediates (not meant to be committed) ---"
rm -f /src/.*.cmd /src/.module-common.o /src/*.mod /src/*.mod.c /src/*.mod.o \
      /src/*.o /src/Module.symvers /src/modules.order

echo "--- verifying vermagic matches across all modules ---"
for ko in /src/az04_codec.ko /src/az04_card.ko /out/snd-soc-core.ko /out/snd-pcm-dmaengine.ko /out/snd-compress.ko; do
    vermagic=$(modinfo -F vermagic "$ko" | awk '{print $1}')
    echo "  $(basename "$ko"): $vermagic"
    if [ "$vermagic" != "$KVER" ]; then
        echo "ERROR: $ko vermagic ($vermagic) doesn't match kernel ($KVER)" >&2
        exit 1
    fi
done
DOCKER_SCRIPT

docker run --rm --platform linux/arm64 \
    -v "$SRC_DIR:/src" \
    -v "$OUT_DIR:/out" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:trixie bash /inner.sh

for f in "$SRC_DIR/az04_codec.ko" "$SRC_DIR/az04_card.ko" \
         "$OUT_DIR/snd-soc-core.ko" "$OUT_DIR/snd-pcm-dmaengine.ko" "$OUT_DIR/snd-compress.ko"; do
    [ -s "$f" ] || { echo "FAILED: expected output missing or empty: $f" >&2; exit 1; }
done

echo ""
echo "Built:"
echo "  $SRC_DIR/az04_codec.ko"
echo "  $SRC_DIR/az04_card.ko"
echo "  $OUT_DIR/snd-soc-core.ko"
echo "  $OUT_DIR/snd-pcm-dmaengine.ko"
echo "  $OUT_DIR/snd-compress.ko"
echo ""
echo "Deployment into an already-booted guest: docs/BUILDING.md's"
echo "'Reimplementing az04-codec as a loadable kernel module' section."