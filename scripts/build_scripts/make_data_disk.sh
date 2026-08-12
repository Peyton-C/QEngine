#!/bin/bash
# Builds the data disk required for booting Engine OS on arm64 / RANE SYSTEM ONE / RMZ2
# Formatting is already handled by Engine itself so this just makes the proper GPT partitions
# with the UUIDs that Engine expects
#
# Usage: make_data_disk.sh [output-path] [--force]
#   Defaults output-path to build/data_disk.img
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

OUT_PATH="$REPO_ROOT/build/data_disk.img"
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) OUT_PATH="$arg" ;;
    esac
done

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

# From usr/lib/systemd/system/data.mount and factory.mount in the
# extracted rootfs (see docs/BUILDING.md) — must match exactly, or
# systemd never finds either partition.
DATA_GUID="d6a62570-4c37-4a42-ae77-8f45bcbfda65"
FACTORY_GUID="f2a055c0-1536-5020-a0c9-3944f89ba52b"

INNER_SCRIPT="$(mktemp /tmp/make-data-disk-inner.XXXXXX.sh)"
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

echo "--- partitioning: GPT, data (1MiB-2049MiB), factory (2049MiB-100%) ---"
# This minimal container has no udev daemon, so parted's post-change
# "udevadm settle" probe logs a harmless "udevadm: not found" each time —
# filtered here since it's noise, not an error (parted's own exit code
# still fails the script via set -e if something actually goes wrong).
parted -s "\$IMG" mklabel gpt 2> >(grep -v udevadm >&2 || true)
parted -s "\$IMG" mkpart data ext4 1MiB 2049MiB 2> >(grep -v udevadm >&2 || true)
parted -s "\$IMG" mkpart factory ext4 2049MiB 100% 2> >(grep -v udevadm >&2 || true)

echo "--- setting partition GUIDs to match RMZ2's data.mount/factory.mount ---"
sgdisk --partition-guid=1:$DATA_GUID --partition-guid=2:$FACTORY_GUID "\$IMG" >/dev/null

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
echo "Both partitions are left unformatted on purpose — RMZ2's"
echo "az0x-data-mkfs/az0x-factory-mkfs services mkfs.ext4 them on first boot."
echo "Attach as a second virtio-blk device per docs/BUILDING.md's QEMU launch section."