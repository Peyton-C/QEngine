#!/bin/bash
# Boot an emulated inMusic device in QEMU. One command line, with the parts that vary
# resolved from the environment rather than by copying the file.
#
# Normally reached through scripts/qemu/run_instance.sh, which reads these values out
# of an instance's instance.env. Run it directly by setting them yourself.
#
#   DEVICE        engine | mpc      which shim stack the guest has, and so whether it
#                                   gets an audio card
#   DISPLAY_MODE  see display_modes.sh
#   ARCH          arm64 | armhf     machine type and device models, see arch_devices.sh
#
# Anything else the wrappers used to hardcode is an override with the same default:
# ROOTFS_IMG, DATA_IMG, KERNEL_IMG, INITRD_IMG, SSH_PORT, VNC_DISPLAY, QEMU_BIN,
# ACCEL, CPU, MEM, SMP, RENDERNODE.
#
# This replaced seven near-identical launcher scripts. They differed only in the two
# values above and in whether the display backend needed GL, and the copies had
# already drifted: two of them disagreed about which audio backend a VNC session uses.
#
# Audio, for the engine family: hda-output (playback-only), NOT hda-duplex. Engine
# marks the first device of each enumeration pass as its default, and the capture
# pass runs second — so when a capture PCM exists it becomes the default, gets
# assigned as the input device, and the playback slot is left null. Engine then
# drives capture only and never feeds playback, which shows up as a stuck XRUN and a
# frozen "Audio_probe" watchdog rather than any error. Playback-only removes the
# ambiguity. Requires alsashim_rmz2.so preloaded into engine.service (see
# build_arm64_rootfs.sh) — without it Engine rejects the card on name alone.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

DEVICE="${DEVICE:-engine}"

# Resolves QEMU_BIN plus the architecture-dependent machine, GPU, input and net
# devices from ARCH — and, for armhf, a lower default MEM than the one below, which
# that architecture's machine type requires. Kept in one file so the highmem
# reasoning behind all of it does not get copied and drift.
# shellcheck source=arch_devices.sh
. "$QEMU_DIR/arch_devices.sh"

# Resolves DISPLAY_ARGS, NEEDS_GL and the audio backend from DISPLAY_MODE.
# shellcheck source=display_modes.sh
. "$QEMU_DIR/display_modes.sh"

# Defaults are the values the wrappers hardcoded, so running any of them directly
# behaves as it did before. Kernel and initrd live under BUILD_DIR because they are
# shared by every instance of an architecture; the disks are per-instance.
case "$ARCH" in
    arm64) _kern="vmlinuz-generic-arm64"; _init="initrd-generic-arm64" ;;
    armhf) _kern="vmlinuz-generic-armhf"; _init="initrd-generic-armhf" ;;
esac
case "$DEVICE" in
    engine) _data="data_disk.img" ;;
    mpc)    _data="emmc.img" ;;
    *) echo "ERROR: unknown DEVICE '$DEVICE' (expected engine or mpc)." >&2; exit 1 ;;
esac

ROOTFS_IMG="${ROOTFS_IMG:-$BUILD_DIR/rootfs_out.img}"
DATA_IMG="${DATA_IMG:-$BUILD_DIR/$_data}"
KERNEL_IMG="${KERNEL_IMG:-$BUILD_DIR/$_kern}"
INITRD_IMG="${INITRD_IMG:-$BUILD_DIR/$_init}"
SSH_PORT="${SSH_PORT:-2225}"
# armhf has already set this to a value its machine type can address; ${MEM:-...}
# leaves that in place, the same way it does for ACCEL and CPU below.
MEM="${MEM:-4096}"
SMP="${SMP:-8}"

# The console. Default multiplexes the QEMU monitor onto stdio, which is what you
# want interactively. Point it at a socket instead (SERIAL=unix:/path,server,nowait)
# to drive the guest console from a script while QEMU runs detached.
SERIAL="${SERIAL:-mon:stdio}"
# Anything else to hand QEMU, word-split. For one-off debugging (a second -serial, a
# -monitor socket, -snapshot) without editing this file.
QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-}"

if [ "$NEEDS_GL" -eq 1 ]; then
    [ -n "$GPU_GL_DEV" ] || {
        echo "ERROR: DISPLAY_MODE=$DISPLAY_MODE needs virgl, which ARCH=$ARCH does" >&2
        echo "       not offer a GPU for. Use a non-GL display mode." >&2
        exit 1; }
    GPU="$GPU_GL_DEV"
else
    GPU="$GPU_DEV"
fi

# By default brew won't add e2fsprogs to the path.
if command -v dumpe2fs >/dev/null 2>&1; then
    DUMPE2FS="dumpe2fs"
else
    DUMPE2FS="/opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs"
fi

# Read the UUID off the image rather than hardcoding it: it is a property of the
# particular extraction, so it changes with every firmware version.
ROOT_UUID="$($DUMPE2FS -h "$ROOTFS_IMG" 2>/dev/null | awk -F': *' '/Filesystem UUID/{print $2}')"
[ -n "$ROOT_UUID" ] || { echo "ERROR: could not read a filesystem UUID from $ROOTFS_IMG" >&2; exit 1; }

# -accel kvm/hvf needs a host of the guest's own architecture, and `-cpu host` is
# accelerator-only, so an x86_64 host can use neither for an ARM guest. arch_devices.sh
# has already pinned TCG for a 32-bit guest, and ${ACCEL:-...} leaves that in place.
case "$(uname -s):$(uname -m)" in
    Darwin:arm64)          ACCEL="${ACCEL:-hvf}"; CPU="${CPU:-host}" ;;
    Linux:aarch64|Linux:arm64) ACCEL="${ACCEL:-kvm}"; CPU="${CPU:-host}" ;;
    *)                     ACCEL="${ACCEL:-tcg}"; CPU="${CPU:-$ARCH_CPU_DEFAULT}" ;;
esac

# Only the engine family gets a card; MPC never had one and does not want one.
AUDIO_ARGS=""
if [ "$DEVICE" = engine ]; then
    AUDIO_ARGS="$(arch_audio_devices "$AUDIODEV_ID") -audiodev $AUDIODEV_BACKEND,id=$AUDIODEV_ID"
fi

# Unquoted on purpose: these hold multiple arguments and must word-split.
# shellcheck disable=SC2086
exec "$QEMU_BIN" \
  -machine "$MACHINE" -accel "$ACCEL" \
  -cpu "$CPU" -m "$MEM" -smp "$SMP" \
  -device "$GPU" \
  $INPUT_DEVS \
  $AUDIO_ARGS \
  -kernel "$KERNEL_IMG" \
  -initrd "$INITRD_IMG" \
  -drive if=none,file="$ROOTFS_IMG",format=raw,id=hd \
  -device virtio-blk-device,drive=hd \
  -drive if=none,file="$DATA_IMG",format=raw,id=data \
  -device virtio-blk-device,drive=data \
  -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 -device "$NET_DEV",netdev=net0 \
  $DISPLAY_ARGS \
  -serial "$SERIAL" \
  $QEMU_EXTRA_ARGS \
  -append "root=UUID=$ROOT_UUID rw rootwait console=ttyAMA0"
