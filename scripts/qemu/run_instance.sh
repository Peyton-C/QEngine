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
#   --gl        render on the host GPU through virgl, promoting the display mode to
#               its GL variant. Already the default where QEMU can serve it.
#   --no-gl     render on the guest CPU, whatever the instance records. The escape
#               hatch when GL itself is suspect.
#               (sdl, sdl-gl, cocoa, vnc, egl-vnc, none — see display_modes.sh)
#   --list      list the instances that exist and exit
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTANCES_DIR="$REPO_ROOT/build/instances"
QEMU_DIR="$REPO_ROOT/scripts/qemu"

NAME=""
DISPLAY_OVERRIDE=""
VIRGL_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --display) DISPLAY_OVERRIDE="$2"; shift 2 ;;
        # --gl / --no-gl move within the display mode's pair (sdl <-> sdl-gl,
        # vnc <-> egl-vnc), so they compose with --display and with whatever the
        # instance recorded. --no-gl is the one to reach for when GL is what is being
        # ruled out; --gl asks for it on an instance created without it.
        --gl) VIRGL_OVERRIDE=on; shift ;;
        --no-gl) VIRGL_OVERRIDE=off; shift ;;
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
# What is printed here is what the instance asked for. display_modes.sh may promote
# or demote it -- for --gl/--no-gl, or because this QEMU cannot serve GL -- and says
# so on its own line when it does.
echo "=== display  : $DISPLAY_MODE${VIRGL_OVERRIDE:+ (virgl $VIRGL_OVERRIDE)}"
echo "=== rootfs   : $ROOTFS_IMG (${ARCH:-arch unrecorded})"
echo "=== kernel   : ${KERNEL_IMG:-<launcher default>}"
echo "=== root UUID: ${ROOT_UUID:-<derived by the launcher>}"
echo "=== ssh      : ssh -p ${SSH_PORT:-2225} root@localhost"

# 5900 + N
_vnc_display="${VNC_DISPLAY:-1}"
case "$_vnc_display" in
    ''|*[!0-9]*) _vnc_port="" ;;
    *)           _vnc_port="$((5900 + _vnc_display))" ;;
esac
# Only print the VNC port if the current display backend is vnc
case "$DISPLAY_MODE" in
    vnc|egl-vnc) ;;
    cocoa)       [ "$VIRGL_OVERRIDE" = on ] || _vnc_port="" ;;
    *)           _vnc_port="" ;;
esac
if [ -n "$_vnc_port" ]; then
    echo "=== vnc      : localhost:$_vnc_port (display :$_vnc_display)"
fi
echo ""

[ -n "$VIRGL_OVERRIDE" ] && export VIRGL="$VIRGL_OVERRIDE"
export ROOTFS_IMG DATA_IMG SSH_PORT VNC_DISPLAY KERNEL_IMG INITRD_IMG ARCH
export DEVICE DISPLAY_MODE
# Optional, and absent from most instance.envs. Exported only when the file set
# it, so arch_devices.sh's own default applies otherwise. An `if` rather than a
# `&&` list because this is the penultimate statement under `set -e`.
if [ -n "${GPU_MAX_OUTPUTS:-}" ]; then
    export GPU_MAX_OUTPUTS
fi
exec "$QEMU_DIR/run_qemu.sh"
