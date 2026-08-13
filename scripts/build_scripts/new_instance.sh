#!/bin/bash
# new_instance.sh — create a self-contained emulator instance, in the spirit of
# an Android AVD: one directory per emulated device, holding its own disks and
# its own configuration, so many devices can be built and run side by side
# without overwriting each other.
#
# Without this, every build writes build/rootfs_out.img and every launcher opens
# that same path on port 2225, so a second device silently clobbers the first and
# a second VM fails to bind (or worse, two QEMUs write one disk).
#
# Usage: new_instance.sh --name <name> --device <engine|mpc> --firmware <image>
#                        [--size <bytes>] [--force]
#   --name      instance name, e.g. rmz2-5.0.4 or mpc-3.9.1
#   --device    which device family the firmware is for:
#                 engine = arm64 / RK3588 Engine OS (RANE SYSTEM ONE)
#                 mpc    = armv7 / RK3288 Akai MPC
#   --firmware  path to the firmware .img to extract
#   --size      rootfs image size in bytes, passed through to the builder
#   --force     rebuild the rootfs even if this instance already has one
#
# The kernel and initrd are deliberately *not* per-instance: they are generic
# distro kernels, identical for every instance of the same architecture. Build
# them once with get_arm64_kernel.sh / get_armv7_kernel.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR="$REPO_ROOT/scripts/build_scripts"
INSTANCES_DIR="$REPO_ROOT/build/instances"

NAME=""
DEVICE=""
FIRMWARE=""
SIZE=""
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --device) DEVICE="$2"; shift 2 ;;
        --firmware) FIRMWARE="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

[ -n "$NAME" ] || { echo "ERROR: --name is required." >&2; exit 1; }
[ -n "$FIRMWARE" ] || { echo "ERROR: --firmware <image> is required." >&2; exit 1; }
[ -f "$FIRMWARE" ] || { echo "ERROR: firmware image not found: $FIRMWARE" >&2; exit 1; }
case "$NAME" in
    */*|"") echo "ERROR: --name must not be empty or contain a slash." >&2; exit 1 ;;
esac

case "$DEVICE" in
    engine)
        ROOTFS_BUILDER="build_arm64_rootfs.sh"
        DISK_BUILDER="make_data_disk.sh"
        DATA_NAME="data_disk.img"
        LAUNCHER="systemone_linux.sh"
        ;;
    mpc)
        ROOTFS_BUILDER="build_mpc_rootfs.sh"
        DISK_BUILDER="make_emmc_disk.sh"
        DATA_NAME="emmc.img"
        LAUNCHER="mpc_linux.sh"
        ;;
    *)
        echo "ERROR: --device must be 'engine' or 'mpc' (got '${DEVICE:-}')." >&2
        exit 1 ;;
esac

command -v dumpe2fs >/dev/null 2>&1 || {
    echo "ERROR: 'dumpe2fs' is required (package e2fsprogs) but not found on PATH." >&2
    echo "On macOS: brew install e2fsprogs, then add its keg-only sbin to PATH." >&2
    exit 1; }

INSTANCE_DIR="$INSTANCES_DIR/$NAME"
ROOTFS_IMG="$INSTANCE_DIR/rootfs.img"
DATA_IMG="$INSTANCE_DIR/$DATA_NAME"
mkdir -p "$INSTANCE_DIR"

echo "=== instance : $NAME"
echo "=== device   : $DEVICE"
echo "=== firmware : $FIRMWARE"
echo "=== directory: $INSTANCE_DIR"
echo ""

### rootfs ####################################################################
ROOTFS_ARGS=(--firmware "$FIRMWARE" --out "$ROOTFS_IMG")
[ -n "$SIZE" ] && ROOTFS_ARGS+=(--size "$SIZE")

# The builders create and resize the image before installing the shim stack, so a
# build that aborts partway leaves a valid-looking but incomplete ext4 image.
# Existence alone therefore does not mean "built" — this marker is written only
# after the builder exits successfully, and an unmarked image is rebuilt rather
# than trusted. Without this, a failed build yields an instance that boots to a
# login prompt with none of the shims, which looks like a working instance.
STAMP="$INSTANCE_DIR/.rootfs.complete"

if [ -e "$ROOTFS_IMG" ] && [ -e "$STAMP" ] && [ "$FORCE" -ne 1 ]; then
    echo "--- rootfs.img exists, keeping it (pass --force to rebuild) ---"
else
    if [ -e "$ROOTFS_IMG" ]; then
        if [ ! -e "$STAMP" ]; then
            echo "--- rootfs.img is present but incomplete (a previous build failed) — rebuilding ---"
        fi
        # The builders refuse to overwrite an existing --out without this.
        ROOTFS_ARGS+=(--force)
    fi
    rm -f "$STAMP"
    "$SCRIPT_DIR/$ROOTFS_BUILDER" "${ROOTFS_ARGS[@]}"
    touch "$STAMP"
fi

### data disk #################################################################
# Never rebuilt by --force: this is where the guest's own state lives, and its
# partition GUIDs are fixed by the guest's mount units, so there is nothing
# version-specific to regenerate. Delete it by hand for a factory-fresh guest.
if [ -e "$DATA_IMG" ]; then
    echo "--- $DATA_NAME exists, keeping it (delete it by hand for a clean /data) ---"
else
    "$SCRIPT_DIR/$DISK_BUILDER" "$DATA_IMG"
fi

### instance.env ##############################################################
# The root filesystem UUID is a property of this particular extraction, so it is
# read off the built image rather than hardcoded — it differs between firmware
# versions, which is why a hardcoded one only ever booted a single build.
ROOT_UUID="$(dumpe2fs -h "$ROOTFS_IMG" 2>/dev/null | awk -F': *' '/Filesystem UUID/{print $2}')"
[ -n "$ROOT_UUID" ] || { echo "ERROR: could not read a filesystem UUID from $ROOTFS_IMG" >&2; exit 1; }

# Host ports are derived from the instance name so two instances never collide,
# deterministically and without a registry file. Override in instance.env if a
# port is already taken on the host.
OFFSET=$(( $(printf '%s' "$NAME" | cksum | cut -d' ' -f1) % 90 + 1 ))

cat > "$INSTANCE_DIR/instance.env" <<EOF
# Generated by new_instance.sh — read by scripts/qemu/run_instance.sh.
# Edit freely; nothing regenerates this file unless you delete it.
INSTANCE_NAME=$NAME
DEVICE=$DEVICE
FIRMWARE_IMG=$FIRMWARE
LAUNCHER=$LAUNCHER
ROOTFS_IMG=$ROOTFS_IMG
DATA_IMG=$DATA_IMG
ROOT_UUID=$ROOT_UUID
SSH_PORT=$(( 2200 + OFFSET ))
VNC_DISPLAY=$OFFSET
EOF

echo ""
echo "Created instance $NAME"
sed 's/^/  /' "$INSTANCE_DIR/instance.env" | grep -v '^  #'
echo ""
KERNEL_HINT="get_arm64_kernel.sh"
[ "$DEVICE" = mpc ] && KERNEL_HINT="get_armv7_kernel.sh"
if [ ! -s "$REPO_ROOT/build/vmlinuz-generic-arm64" ] && [ "$DEVICE" = engine ]; then
    echo "NOTE: no shared kernel yet — run scripts/build_scripts/$KERNEL_HINT first."
elif [ ! -s "$REPO_ROOT/build/vmlinuz-generic-armhf" ] && [ "$DEVICE" = mpc ]; then
    echo "NOTE: no shared kernel yet — run scripts/build_scripts/$KERNEL_HINT first."
fi
echo "Boot it with:"
echo "  scripts/qemu/run_instance.sh --name $NAME"
