#!/bin/bash
# run_instance.sh — boot one instance created by new_instance.sh.
#
# This is a thin wrapper, deliberately: it reads the instance's instance.env,
# exports those values, and execs the ordinary per-device launcher. The QEMU
# command lines stay in exactly one place each (systemone_*.sh, mpc_*.sh), which
# is why those scripts take their paths and ports from the environment with the
# old hardcoded values as defaults.
#
# Usage: run_instance.sh --name <name> [--launcher <script>] [--list]
#   --name      instance under build/instances/ to boot
#   --launcher  override the launcher recorded in instance.env, e.g. to pick a
#               different display backend (systemone_vnc.sh, mpc_macos.sh, ...)
#   --list      list the instances that exist and exit
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTANCES_DIR="$REPO_ROOT/build/instances"
QEMU_DIR="$REPO_ROOT/scripts/qemu"

NAME=""
LAUNCHER_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --launcher) LAUNCHER_OVERRIDE="$2"; shift 2 ;;
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

[ -n "${LAUNCHER_OVERRIDE}" ] && LAUNCHER="$LAUNCHER_OVERRIDE"
[ -x "$QEMU_DIR/$LAUNCHER" ] || { echo "ERROR: launcher not found: $QEMU_DIR/$LAUNCHER" >&2; exit 1; }

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
echo "=== launcher : $LAUNCHER"
echo "=== rootfs   : $ROOTFS_IMG"
echo "=== root UUID: ${ROOT_UUID:-<derived by the launcher>}"
echo "=== ssh      : ssh -p ${SSH_PORT:-2225} root@localhost"
echo ""

export ROOTFS_IMG DATA_IMG SSH_PORT VNC_DISPLAY
exec "$QEMU_DIR/$LAUNCHER"
