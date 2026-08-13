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
# Usage: new_instance.sh --name <name> --firmware <image>
#                        [--device <engine|mpc>] [--size <bytes>] [--force]
#   --name      instance name, e.g. rmz2-5.0.4 or mpc-3.9.1
#   --device    which device family the firmware is for. Optional: when omitted it
#               is identified from the firmware itself, which costs one extra
#               extraction (a few seconds). Pass it to skip that, or to state the
#               intent explicitly — a wrong value is still caught either way.
#                 engine = Engine OS (RANE SYSTEM ONE and relatives)
#                 mpc    = Akai MPC
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

# The same extraction the rootfs builders use, so identifying a firmware's family
# before choosing between them does not mean a third copy of the logic.
# shellcheck source=extract_rootfs.sh
. "$SCRIPT_DIR/extract_rootfs.sh"
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

# Which launcher to record depends on the host OS, not on the device: the
# *_linux.sh scripts use KVM-or-TCG with GTK and PipeWire, the *_macos.sh ones HVF
# with Cocoa and CoreAudio. Pinning the Linux one here would hand a Mac a command
# line macOS rejects outright (`-accel kvm: invalid accelerator`).
case "$(uname -s)" in
    Darwin) HOST_OS="macos" ;;
    *)      HOST_OS="linux" ;;
esac

# Device family registry. One row per family:
#
#   <family>|<rootfs markers>|<rootfs builder>|<disk builder>|<data image>|<launcher prefix>
#
# The markers are paths that must ALL exist in the built rootfs for the row to
# match, space separated, so a family that needs more than one piece of evidence
# just lists more. Rows are tried in order, which lets a future family whose
# markers are a superset of another's be listed first and win. Adding a device
# family means adding a row; none of the logic below changes.
#
# The markers are what the family actually is, not a guess from the filename or the
# product code: /usr/Engine is the Engine install tree, /usr/bin/MPC is the MPC
# application binary itself.
DEVICE_FAMILIES="\
engine|/usr/Engine|build_arm64_rootfs.sh|make_data_disk.sh|data_disk.img|systemone
mpc|/usr/bin/MPC|build_mpc_rootfs.sh|make_emmc_disk.sh|emmc.img|mpc"

family_names() { printf '%s\n' "$DEVICE_FAMILIES" | cut -d'|' -f1 | tr '\n' ' '; }

# Look a family up by name, printing its row. Empty output means it is not known.
family_row() {
    # An `if` rather than `cond && cmd`: the loop's exit status is its last
    # iteration's, so a non-matching final row would fail the pipeline and, under
    # `set -e` with pipefail, abort the script silently.
    printf '%s\n' "$DEVICE_FAMILIES" | while IFS='|' read -r fam rest; do
        if [ "$fam" = "$1" ]; then printf '%s|%s\n' "$fam" "$rest"; fi
    done
}

# Identify a built rootfs by its markers, printing the family name. Empty output
# means nothing matched, i.e. a device family with no row yet.
detect_family() {
    printf '%s\n' "$DEVICE_FAMILIES" | while IFS='|' read -r fam markers _; do
        [ -n "$fam" ] || continue
        matched=1
        for marker in $markers; do
            "$DEBUGFS" -R "stat $marker" "$1" 2>/dev/null | grep -q 'Inode:' || { matched=0; break; }
        done
        if [ "$matched" -eq 1 ]; then printf '%s\n' "$fam"; break; fi
    done
}

# brew keeps e2fsprogs keg-only, so dumpe2fs is off PATH on a stock macOS setup.
# Same fallback the macOS launchers already use, so this does not become the one
# step that needs PATH surgery first.
if command -v dumpe2fs >/dev/null 2>&1; then
    DUMPE2FS="dumpe2fs"; DEBUGFS="debugfs"
elif [ -x /opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs ]; then
    DUMPE2FS="/opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs"
    DEBUGFS="/opt/homebrew/opt/e2fsprogs/sbin/debugfs"
else
    echo "ERROR: 'dumpe2fs' is required (package e2fsprogs) but not found on PATH." >&2
    echo "On macOS: brew install e2fsprogs, then add its keg-only sbin to PATH." >&2
    exit 1
fi

# --device is optional. It is needed only to choose the rootfs builder, and that
# choice can be made from the firmware instead — the builders are what perform the
# extraction, so identifying the family first means extracting once up front.
#
# Order of preference: what was asked for, then what this instance was built as
# before (free — its rootfs is already on disk), then the firmware itself.
if [ -z "$DEVICE" ] && [ -f "$INSTANCES_DIR/$NAME/instance.env" ]; then
    DEVICE="$(grep '^DEVICE=' "$INSTANCES_DIR/$NAME/instance.env" | cut -d= -f2)"
    [ -n "$DEVICE" ] && echo "--- reusing this instance's recorded device family: $DEVICE ---"
fi

if [ -z "$DEVICE" ]; then
    echo "--- no --device given, identifying $FIRMWARE ---"
    PROBE_IMG="$(mktemp /tmp/qengine-probe.XXXXXX.img)"
    trap 'rm -f "$PROBE_IMG"' EXIT
    extract_rootfs "$FIRMWARE" "$PROBE_IMG" >/dev/null
    DEVICE="$(detect_family "$PROBE_IMG")"
    rm -f "$PROBE_IMG"
    trap - EXIT
    [ -n "$DEVICE" ] || {
        echo "ERROR: $FIRMWARE matches no known device family." >&2
        echo "       Looked for the markers of: $(family_names)" >&2
        echo "       If this is a new family, add a row to DEVICE_FAMILIES in this script." >&2
        exit 1; }
    echo "--- identified as $DEVICE ---"
fi

DEVICE_ROW="$(family_row "$DEVICE")"
[ -n "$DEVICE_ROW" ] || {
    echo "ERROR: --device must be one of: $(family_names)(got '$DEVICE')." >&2
    exit 1; }

IFS='|' read -r _ DEVICE_MARKERS ROOTFS_BUILDER DISK_BUILDER DATA_NAME LAUNCHER_PREFIX <<EOF
$DEVICE_ROW
EOF
LAUNCHER="${LAUNCHER_PREFIX}_${HOST_OS}.sh"

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

### device family #############################################################
# Confirm the firmware really is the family that was asked for, now that there is a
# filesystem to look at. --device still has to be given up front, because it selects
# the builder that performs the extraction, but it no longer has to be trusted after
# the fact.
#
# This catches a mismatch the architecture guards cannot. Those compare the rootfs
# against their own builder's architecture, so engine-vs-mpc confusion is only caught
# while the two families happen to differ in architecture. An arm64 MPC image built
# as --device engine passes them and gets the entire Engine shim stack installed into
# an MPC rootfs, producing an instance that boots and can never work.
DETECTED_FAMILY="$(detect_family "$ROOTFS_IMG")"
if [ -z "$DETECTED_FAMILY" ]; then
    echo "ERROR: $ROOTFS_IMG matches no known device family." >&2
    echo "       Looked for the markers of: $(family_names)" >&2
    echo "       If this is a new family, add a row to DEVICE_FAMILIES in this script." >&2
    exit 1
elif [ "$DETECTED_FAMILY" != "$DEVICE" ]; then
    echo "ERROR: --device $DEVICE was asked for, but this rootfs identifies as $DETECTED_FAMILY" >&2
    echo "       (looked for $DEVICE_MARKERS and did not find it)." >&2
    echo "       It has been built with the wrong shim stack; rebuild with" >&2
    echo "       --device $DETECTED_FAMILY --force." >&2
    exit 1
fi
echo "--- rootfs is $DETECTED_FAMILY ---"

### data disk #################################################################
# Never rebuilt by --force: this is where the guest's own state lives, and its
# partition GUIDs are fixed by the guest's mount units, so there is nothing
# version-specific to regenerate. Delete it by hand for a factory-fresh guest.
if [ -e "$DATA_IMG" ]; then
    echo "--- $DATA_NAME exists, keeping it (delete it by hand for a clean /data) ---"
else
    "$SCRIPT_DIR/$DISK_BUILDER" "$DATA_IMG"
fi

### kernel ####################################################################
# The kernel has to match the rootfs's architecture, and that is not implied by
# --device: Engine OS ships on both RK3288 (armv7) and RK3588 (arm64), and so does
# MPC. Nor is it implied by the container format -- the AZ0x set spans both. So it
# is read off the built filesystem instead of guessed from a product-code table:
# the dynamic loader's name is architecture-specific and unambiguous.
#
# Probing after the rootfs build rather than before is what makes this work at all:
# the kernel is not needed until boot, so by the time it is chosen the filesystem
# that decides it already exists.
if "$DEBUGFS" -R "stat /lib/ld-linux-aarch64.so.1" "$ROOTFS_IMG" 2>/dev/null | grep -q 'Inode:'; then
    ARCH="arm64"
elif "$DEBUGFS" -R "stat /lib/ld-linux-armhf.so.3" "$ROOTFS_IMG" 2>/dev/null | grep -q 'Inode:'; then
    ARCH="armhf"
else
    echo "ERROR: could not tell the architecture of $ROOTFS_IMG -- no known dynamic loader." >&2
    exit 1
fi

case "$ARCH" in
    arm64) KERNEL_BUILDER="get_arm64_kernel.sh" ;;
    armhf) KERNEL_BUILDER="get_armv7_kernel.sh" ;;
esac
KERNEL_IMG="$REPO_ROOT/build/vmlinuz-generic-$ARCH"
INITRD_IMG="$REPO_ROOT/build/initrd-generic-$ARCH"

echo "--- rootfs is $ARCH ---"

# Built on demand rather than left as an instruction to follow: it is shared by
# every instance of this architecture, so this happens once and is a no-op after.
if [ ! -s "$KERNEL_IMG" ] || [ ! -s "$INITRD_IMG" ]; then
    echo "--- no $ARCH kernel yet, building it (once per architecture) ---"
    "$SCRIPT_DIR/$KERNEL_BUILDER"
    [ -s "$KERNEL_IMG" ] && [ -s "$INITRD_IMG" ] || {
        echo "ERROR: $KERNEL_BUILDER did not produce $KERNEL_IMG and $INITRD_IMG." >&2
        exit 1; }
else
    echo "--- reusing the existing $ARCH kernel ---"
fi

# The launchers take their machine type and device models from ARCH, so either
# architecture boots. Only the combination is new: an armv7 Engine or an arm64 MPC
# has never been run, and the audio path in particular differs (mmio virtio-sound
# rather than PCI hda). Flag it as unproven rather than implying it is broken.
case "$LAUNCHER:$ARCH" in
    systemone_*:arm64|mpc_*:armhf) ;;
    *) echo "NOTE: $DEVICE on $ARCH is an untried combination. $LAUNCHER will build a"
       echo "      $ARCH command line for it, but nothing here has booted one yet." ;;
esac

### instance.env ##############################################################
# The root filesystem UUID is a property of this particular extraction, so it is
# read off the built image rather than hardcoded — it differs between firmware
# versions, which is why a hardcoded one only ever booted a single build.
ROOT_UUID="$("$DUMPE2FS" -h "$ROOTFS_IMG" 2>/dev/null | awk -F': *' '/Filesystem UUID/{print $2}')"
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
ARCH=$ARCH
KERNEL_IMG=$KERNEL_IMG
INITRD_IMG=$INITRD_IMG
SSH_PORT=$(( 2200 + OFFSET ))
VNC_DISPLAY=$OFFSET
EOF

echo ""
echo "Created instance $NAME"
sed 's/^/  /' "$INSTANCE_DIR/instance.env" | grep -v '^  #'
echo ""
echo "Boot it with:"
echo "  scripts/qemu/run_instance.sh --name $NAME"
