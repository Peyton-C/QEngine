#!/bin/bash
# run_instance.sh — boot one instance created by new_instance.sh.
#
# This is a thin wrapper, deliberately: it reads the instance's instance.env, exports
# those values, and execs run_qemu.sh. The QEMU command line stays in exactly one
# place, which is why run_qemu.sh takes everything that varies from the environment.
#
# Usage: run_instance.sh --name <name> [--display <mode>] [--list]
#   --name      instance under build/instances/ to boot
#   --display   override the display backend recorded in instance.env for this run
#               (sdl, sdl-gl, cocoa, vnc, egl-vnc, none — see display_modes.sh)
#   --list      list the instances that exist and exit
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTANCES_DIR="$REPO_ROOT/build/instances"
QEMU_DIR="$REPO_ROOT/scripts/qemu"

NAME=""
DISPLAY_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --display) DISPLAY_OVERRIDE="$2"; shift 2 ;;
        --list)
            if [ -d "$INSTANCES_DIR" ]; then
                for d in "$INSTANCES_DIR"/*/; do
                    [ -f "$d/instance.env" ] || continue
                    # shellcheck disable=SC1091
                    dev=$(grep '^DEVICE=' "$d/instance.env" | cut -d= -f2)
                    ssh=$(grep '^SSH_PORT=' "$d/instance.env" | cut -d= -f2)
                    printf '  %-24s device=%-7s ssh=%s\n' "$(basename "$d")" "$dev" "$ssh"
                done
            fi
            exit 0 ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

if [ -z "$NAME" ]; then
    echo "ERROR: --name is required. Existing instances:" >&2
    "$0" --list >&2
    exit 1
fi

INSTANCE_DIR="$INSTANCES_DIR/$NAME"
ENV_FILE="$INSTANCE_DIR/instance.env"
if [ ! -f "$ENV_FILE" ]; then
    echo "ERROR: $ENV_FILE not found — create the instance first:" >&2
    echo "  scripts/build_scripts/new_instance.sh --name $NAME --device <engine|mpc> --firmware <image>" >&2
    exit 1
fi

# shellcheck source=/dev/null
. "$ENV_FILE"

[ -n "$DISPLAY_OVERRIDE" ] && DISPLAY_MODE="$DISPLAY_OVERRIDE"

# instance.env used to record a launcher filename instead of a display mode. Map the
# old key rather than failing, so an instance created before that change still boots.
if [ -z "${DISPLAY_MODE:-}" ] && [ -n "${LAUNCHER:-}" ]; then
    case "$LAUNCHER" in
        *_virgl.sh)  DISPLAY_MODE="sdl-gl" ;;
        *_vnc.sh)    DISPLAY_MODE="vnc" ;;
        *_macos.sh)  DISPLAY_MODE="cocoa" ;;
        *)           DISPLAY_MODE="sdl" ;;
    esac
    echo "NOTE: instance.env records LAUNCHER=$LAUNCHER, which predates DISPLAY_MODE."
    echo "      Using DISPLAY_MODE=$DISPLAY_MODE. Replace the key to silence this."
fi
DISPLAY_MODE="${DISPLAY_MODE:-sdl}"

for f in "$ROOTFS_IMG" "$DATA_IMG"; do
    [ -s "$f" ] || { echo "ERROR: required image missing or empty: $f" >&2; exit 1; }
done

# Refuse to run two QEMUs against one set of disks. docs/BUILDING.md warns twice
# that two writers on one image corrupts the filesystem, and it fails silently
# rather than at startup. The fd stays open across the exec below, so the lock is
# held for the VM's lifetime and released when it exits. flock is Linux-only, so
# on macOS QEMU's own image locking is the only guard.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$INSTANCE_DIR/.run.lock"
    if ! flock -n 9; then
        echo "ERROR: instance $NAME is already running." >&2
        exit 1
    fi
fi

echo "=== instance : $NAME (${DEVICE:-?}, from $(basename "${FIRMWARE_IMG:-unknown}"))"
echo "=== display  : $DISPLAY_MODE"
echo "=== rootfs   : $ROOTFS_IMG (${ARCH:-arch unrecorded})"
echo "=== kernel   : ${KERNEL_IMG:-<launcher default>}"
echo "=== root UUID: ${ROOT_UUID:-<derived by the launcher>}"
echo "=== ssh      : ssh -p ${SSH_PORT:-2225} root@localhost"
echo ""

export ROOTFS_IMG DATA_IMG SSH_PORT VNC_DISPLAY KERNEL_IMG INITRD_IMG ARCH
export DEVICE DISPLAY_MODE
exec "$QEMU_DIR/run_qemu.sh"
