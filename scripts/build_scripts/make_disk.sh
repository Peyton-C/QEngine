#!/bin/bash
# Builds the writable disk a guest needs alongside its rootfs. Formatting is handled
# by the guest itself on first boot, so this only lays down the GPT partitions with the
# names and GUIDs the guest's own mount units look for.
#
# Usage: make_disk.sh --family <engine|mpc> [output-path] [--force]
#
# This was two scripts, make_data_disk.sh and make_emmc_disk.sh, differing in 24 lines
# of 216: the partition table and the wording around it. That table is now the
# PARTITIONS array below, one entry per partition, and the old names remain as
# wrappers since BUILD_ARM64.md and BUILD_MPC.md tell readers to run them.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

FAMILY=""
OUT_PATH=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --family) FAMILY="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) OUT_PATH="$1"; shift ;;
    esac
done

# One entry per partition: "<name>:<start>:<end>:<GUID>". The GUIDs come from the
# guest's own mount units in the extracted rootfs (see docs/BUILDING.md) and must
# match exactly, or systemd never finds the partition.
case "$FAMILY" in
    engine)
        # usr/lib/systemd/system/data.mount and factory.mount on RMZ2.
        PARTITIONS=(
            "data:1MiB:2049MiB:d6a62570-4c37-4a42-ae77-8f45bcbfda65"
            "factory:2049MiB:100%:f2a055c0-1536-5020-a0c9-3944f89ba52b"
        )
        DEFAULT_NAME="data_disk.img"
        MKFS_SERVICES="az0x-data-mkfs/az0x-factory-mkfs services"
        ;;
    mpc)
        # A single /data partition, bind-mounted to /media/az01-internal.
        PARTITIONS=(
            "az01-internal:1MiB:100%:931ad49d-ad59-0849-833a-9bf00af5b60e"
        )
        DEFAULT_NAME="emmc.img"
        MKFS_SERVICES="az0x-data-mkfs service"
        ;;
    *)
        echo "ERROR: --family must be 'engine' or 'mpc' (got '${FAMILY:-}')." >&2
        exit 1 ;;
esac

[ -n "$OUT_PATH" ] || OUT_PATH="$REPO_ROOT/build/$DEFAULT_NAME"

if [ -e "$OUT_PATH" ] && [ "$FORCE" -ne 1 ]; then
    echo "ERROR: $OUT_PATH already exists — refusing to overwrite (pass --force to replace it)." >&2
    exit 1
fi

OUT_DIR="$(cd "$(dirname "$OUT_PATH")" && pwd)"
OUT_NAME="$(basename "$OUT_PATH")"
mkdir -p "$OUT_DIR"

# Nothing here is architecture-specific (parted/sgdisk on an image file), so run
# natively. Pinned explicitly because Docker caches images under a bare tag
# regardless of the platform they were pulled for: once something has pulled
# debian:bookworm-slim for arm64, a bare `docker run` reuses that and runs the
# whole thing emulated for no reason.
case "$(uname -m)" in
    x86_64|amd64)   HOST_PLATFORM="linux/amd64" ;;
    aarch64|arm64)  HOST_PLATFORM="linux/arm64" ;;
    *)              HOST_PLATFORM="" ;;
esac

# Build the parted and sgdisk invocations from the table. Generated out here rather
# than looped inside the container script so the container still runs a flat list of
# commands under `set -e`, exactly as it did when the two partition layouts were
# written out by hand.
PARTED_LINES=""
SGDISK_ARGS=""
PART_DESC=""
_n=0
for _part in "${PARTITIONS[@]}"; do
    _n=$((_n + 1))
    IFS=':' read -r _name _start _end _guid <<EOF
$_part
EOF
    PARTED_LINES="${PARTED_LINES}parted -s \"\$IMG\" mkpart $_name ext4 $_start $_end 2> >(grep -v udevadm >&2 || true)
"
    SGDISK_ARGS="$SGDISK_ARGS --partition-guid=$_n:$_guid"
    PART_DESC="${PART_DESC:+$PART_DESC, }$_name ($_start-$_end)"
done

INNER_SCRIPT="$(mktemp /tmp/make-disk-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

# Written to a real file and mounted in, not piped via stdin — the other
# scripts in this directory found that pattern silently swallows the
# container's stdout under this host's Docker setup.
cat > "$INNER_SCRIPT" <<DOCKER_SCRIPT
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq parted gdisk >/dev/null 2>&1

IMG=/out/$OUT_NAME
truncate -s 4G "\$IMG"

echo "--- partitioning: GPT, $PART_DESC ---"
# This minimal container has no udev daemon, so parted's post-change
# "udevadm settle" probe logs a harmless "udevadm: not found" each time —
# filtered here since it's noise, not an error (parted's own exit code
# still fails the script via set -e if something actually goes wrong).
parted -s "\$IMG" mklabel gpt 2> >(grep -v udevadm >&2 || true)
${PARTED_LINES}
echo "--- setting partition GUIDs to match the guest's mount units ---"
sgdisk$SGDISK_ARGS "\$IMG" >/dev/null

echo "--- resulting partition table ---"
sgdisk -p "\$IMG"

if [ ! -s "\$IMG" ]; then
    echo "ERROR: output file missing or empty — something failed silently above." >&2
    exit 1
fi

# The image is created in here, so it lands owned by the container's root. QEMU
# opens its disks read-write, so leaving it root-owned makes the VM fail to
# start with "Could not open ...: Permission denied". Hand it back to the user
# who invoked the script.
if [ -n "\${HOST_UID:-}" ] && [ -n "\${HOST_GID:-}" ]; then
    chown "\$HOST_UID:\$HOST_GID" "\$IMG"
fi
DOCKER_SCRIPT

echo "Building $OUT_PATH ..."

docker run --rm \
    ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$OUT_DIR:/out" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:bookworm-slim bash /inner.sh

if [ ! -s "$OUT_PATH" ]; then
    echo "FAILED: expected output file is missing from $OUT_PATH — see output above for where the container script stopped." >&2
    exit 1
fi

echo ""
echo "Built: $OUT_PATH"
echo ""
echo "The partitions are left unformatted on purpose — the guest's"
echo "$MKFS_SERVICES mkfs.ext4 them on first boot."
echo "Attach as a second virtio-blk device per docs/BUILDING.md's QEMU launch section."