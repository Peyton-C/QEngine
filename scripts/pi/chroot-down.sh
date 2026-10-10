#!/bin/bash
# chroot-down.sh — stops Engine and takes the chroot down, in an order that
# leaves nothing half-written: what chroot-engine.sh and chroot-up.sh did,
# undone. qengine.service runs it when the unit is stopped, which includes the
# machine shutting down or rebooting.
#
# Usage: sudo chroot-down.sh
#   Safe to run with nothing running or mounted. Afterwards chroot-up.sh and
#   chroot-engine.sh (or `systemctl start qengine`) bring everything back.
#
# The order, and why:
#   1. Engine, asked to quit and waited for. quitshim is what makes SIGTERM a
#      request and not a kill; without it in the rootfs this is no gentler than
#      before. Everything Engine talks to is still running at this point.
#   2. touchbridge and midisurface, which only ever served Engine.
#   3. edisksd, then every drive it mounted. It does not unmount on its way
#      out, and a USB drive is the filesystem here least able to take being
#      dropped.
#   4. whatever else still lives in the chroot (D-Bus, helpers Engine left).
#   5. the mounts, /data and the rootfs images last.
#
# Environment (or $QENGINE_DIR/pi.env): QENGINE_DIR, ENGINE_ROOT as in
# chroot-up.sh.
set -uo pipefail

QENGINE_DIR="${QENGINE_DIR:-$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/qengine}"
# Settings file first, then the caller's environment again on top of it, so that
# `sudo AUDIO_CARD=... chroot-engine.sh` overrides the file for one run.
_caller_env="$(export -p)"
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
eval "$_caller_env"
R="${ENGINE_ROOT:-/srv/engine}"

# wait_gone <seconds> <pgrep -f pattern>
wait_gone() {
    for _ in $(seq 1 $(($1 * 10))); do
        pgrep -f "$2" >/dev/null || return 0
        sleep 0.1
    done
    return 1
}

# Anchored patterns, as in chroot-engine.sh.
#
# The launch script and powerkey.py before Engine itself: both act on how
# Engine exits, and powering the machine off is among the things they do.
pkill -f '/powerkey\.py '
pkill -f '^/bin/sh /usr/Engine/Scripts/'
if pgrep -f '^/usr/Engine/Engine' >/dev/null; then
    pkill -f '^/usr/Engine/Engine'
    if wait_gone 20 '^/usr/Engine/Engine'; then
        echo "Engine quit"
    else
        echo "WARNING: Engine did not quit within 20s; killing it." >&2
        pkill -9 -f '^/usr/Engine/Engine'
    fi
fi

pkill -f '^/root/touchbridge'; pkill -f '^/root/midisurface'
pkill -f '^/usr/libexec/edisksd'
wait_gone 5 '^/usr/libexec/edisksd' || pkill -9 -f '^/usr/libexec/edisksd'

if mountpoint -q "$R"; then
    # By root directory and not by name, so that nothing is left holding the
    # mounts open. That includes a shell someone has open in the chroot.
    in_chroot() {
        local p
        for p in /proc/[0-9]*; do
            [ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && echo "${p#/proc/}"
        done
    }
    pids="$(in_chroot)"
    if [ -n "$pids" ]; then
        # shellcheck disable=SC2086  # a list of pids
        kill $pids 2>/dev/null
        for _ in $(seq 1 30); do [ -z "$(in_chroot)" ] && break; sleep 0.1; done
        pids="$(in_chroot)"
        # shellcheck disable=SC2086
        [ -z "$pids" ] || kill -9 $pids 2>/dev/null
    fi

    sync
    # /sys and /dev are the host's own, bound in, and mounts propagate between
    # the two copies by default: unmounting the chroot's /sys/fs/cgroup would
    # unmount the machine's. Cutting the chroot's side loose first prevents it.
    mount --make-rslave "$R"
    if umount -R "$R"; then
        echo "chroot at $R unmounted"
    else
        echo "WARNING: could not unmount $R; still mounted:" >&2
        findmnt -R -n -o TARGET "$R" >&2
        exit 1
    fi
fi

# chroot-up.sh took the login prompt off the display. Not while the machine is
# going down, when asking for a unit to start only gets in the way.
if [ "$(systemctl is-system-running 2>/dev/null)" != stopping ]; then
    systemctl start --no-block getty@tty1.service 2>/dev/null || true
fi
