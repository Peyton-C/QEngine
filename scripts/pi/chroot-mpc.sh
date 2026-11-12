#!/bin/bash
# chroot-mpc.sh — runs a Gen 2 MPC instance's rootfs in a chroot on an arm64
# Linux host, the way chroot-up.sh and chroot-engine.sh do for Engine.
#
# Usage: sudo chroot-mpc.sh [up|start|stop|down]
#   up     mount the rootfs, its data disk and the host's /proc, /sys, /dev; load
#          snd-dummy. Idempotent, and start does it when needed.
#   start  start, or restart, MPC (the default).
#   stop   stop MPC, leaving the chroot mounted.
#   down   stop everything in the chroot and unmount it.
#
# The instance's own rootfs.img and data_disk.img are used as they are: what QEMU
# needed is already in them (dtshim and its fake devicetree, mpcsurface,
# alsashim, touchbridge_mpc), and nothing about the chroot needs more. Copy them
# from build/instances/<name>/ into $MPC_DIR, or let stage_for_pi.sh do it.
#
# Environment (or $QENGINE_DIR/pi.env, shared with the Engine scripts):
#   QENGINE_DIR  Default: ~/qengine of the user who ran sudo.
#   MPC_DIR      where rootfs.img and data_disk.img are. Default: $QENGINE_DIR/mpc
#   MPC_ROOT     where the rootfs is mounted. Default: /srv/mpc
#   AUDIO_CARD   where MPC's main output is heard: an ALSA id from
#                /proc/asound/cards, or a name in /dev/snd/by-path. Default: the
#                first USB audio card, else the first card that is not snd-dummy.
#   MPC_TB_ARGS      touchbridge options. Default: none with a QEMU tablet (a VM),
#                `--mouse` otherwise, which turns a USB mouse into the touchscreen.
#   ALSASHIM_METER=1  log the output's peak level in mpc.log every second.
#
# Logs are in $MPC_ROOT/run/: mpc.log, touchbridge.log, mpcsurface.log. The panel
# takes commands on $MPC_ROOT/run/mpcsurface.fifo, as in a guest.
#
# Engine and MPC both drive the display directly, so run one at a time.
set -uo pipefail

QENGINE_DIR="${QENGINE_DIR:-$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/qengine}"
_caller_env="$(export -p)"
# shellcheck disable=SC1091
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
eval "$_caller_env"
MPC_DIR="${MPC_DIR:-$QENGINE_DIR/mpc}"
R="${MPC_ROOT:-/srv/mpc}"
BASE=(PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root)

[ "$(id -u)" = 0 ] || { echo "ERROR: run as root (sudo)." >&2; exit 1; }

# By root directory and not by name: the rootfs's dbus-daemon has the same
# command line as a host's, and crashpad_handler is MPC's, started by it.
in_chroot() {
    local p
    for p in /proc/[0-9]*; do
        [ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && echo "${p#/proc/}"
    done
}

# By command line: MPC names its main thread "MPC 3 Main".
mpc_pid() {
    local p
    for p in $(pgrep -f '^/usr/bin/MPC'); do
        [ "$(readlink "/proc/$p/root" 2>/dev/null)" = "$R" ] && echo "$p"
    done
}

up() {
    [ -f "$MPC_DIR/rootfs.img" ] && [ -f "$MPC_DIR/data_disk.img" ] || {
        echo "ERROR: no rootfs.img and data_disk.img in $MPC_DIR (set MPC_DIR)." >&2; exit 1; }
    mkdir -p "$R"
    # Read-only, as on the device. Everything MPC writes goes to /data or a tmpfs.
    mountpoint -q "$R" || mount -o loop,ro "$MPC_DIR/rootfs.img" "$R" || exit 1
    [ -x "$R/usr/bin/MPC" ] && [ -e "$R/lib/ld-linux-aarch64.so.1" ] || {
        echo "ERROR: $MPC_DIR/rootfs.img is not a Gen 2 (arm64) MPC rootfs." >&2
        umount "$R"; exit 1; }

    # data_disk.img is /data and /factory, the two partitions new_instance.sh
    # gave it. The vendor mount units find them by PARTUUID, which the host's
    # loop devices also carry, but there is no systemd in here to read the units.
    local loop
    loop="$(losetup -j "$MPC_DIR/data_disk.img" | cut -d: -f1 | head -n 1)"
    [ -n "$loop" ] || loop="$(losetup -fP --show "$MPC_DIR/data_disk.img")"
    mountpoint -q "$R/data"    || mount "${loop}p1" "$R/data"
    mountpoint -q "$R/factory" || mount "${loop}p2" "$R/factory"

    mountpoint -q "$R/proc" || mount -t proc proc "$R/proc"
    mountpoint -q "$R/sys"  || mount --rbind /sys "$R/sys"
    mountpoint -q "$R/dev"  || mount --rbind /dev "$R/dev"
    # So that unmounting the chroot's copies of /sys and /dev never unmounts the
    # host's own submounts of them (hugepages, mqueue, debugfs, ...), which a
    # shared bind does by default.
    mount --make-rslave "$R/sys"
    mount --make-rslave "$R/dev"
    mountpoint -q "$R/run"  || mount -t tmpfs -o mode=0755 tmpfs "$R/run"
    mkdir -p "$R/run/udev"
    mountpoint -q "$R/run/udev" || mount --bind /run/udev "$R/run/udev"
    mountpoint -q "$R/media" || mount -t tmpfs -o mode=1777 tmpfs "$R/media"
    mountpoint -q "$R/tmp"   || mount -t tmpfs -o mode=1777 tmpfs "$R/tmp"
    mountpoint -q "$R/var/volatile" || mount -t tmpfs tmpfs "$R/var/volatile"
    mkdir -p "$R/var/volatile/log" "$R/var/volatile/tmp"
    # var-lib.mount on the device.
    mkdir -p "$R/data/system/var-lib"
    mountpoint -q "$R/var/lib" || mount --bind "$R/data/system/var-lib" "$R/var/lib"

    # MPC's internal card, as qengine-mpc-audio.service gives it in a guest: a
    # duplex card that keeps time at 128-frame periods, opened by udev path.
    # Distribution kernels build snd-dummy; the rule is the guest's, on the host,
    # since the chroot's /dev is the host's.
    modprobe -a uinput snd-seq snd-rawmidi 2>/dev/null
    modprobe snd-dummy || { echo "ERROR: this kernel has no snd-dummy." >&2; exit 1; }
    local rule=/etc/udev/rules.d/90-qengine-mpc-audio.rules
    local want='SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{id}=="Dummy", SYMLINK+="snd/by-path/platform-sound"'
    if [ "$(cat "$rule" 2>/dev/null)" != "$want" ]; then
        printf '%s\n' "$want" > "$rule"
        udevadm control --reload
        udevadm trigger --subsystem-match=sound --action=change
        udevadm settle
    fi
    [ -e /dev/snd/by-path/platform-sound ] || {
        echo "ERROR: snd-dummy loaded but /dev/snd/by-path/platform-sound is missing." >&2; exit 1; }

    # The login prompt on tty1 shares the display MPC renders to.
    systemctl stop getty@tty1.service 2>/dev/null || true
    echo "chroot ready at $R"
}

stop() {
    local p
    for p in $(pgrep -f 'mpc-supervisor$'); do
        [ "$(readlink "/proc/$p/root" 2>/dev/null)" = "$R" ] && kill "$p"
    done
    p="$(mpc_pid)"
    if [ -n "$p" ]; then
        # MPC exits within half a second of SIGTERM and does not count it as an
        # unclean shutdown at the next start (3.10.0, ACVA3).
        # shellcheck disable=SC2086
        kill $p
        for _ in $(seq 1 50); do [ -z "$(mpc_pid)" ] && break; sleep 0.1; done
        p="$(mpc_pid)"
        # shellcheck disable=SC2086
        [ -z "$p" ] || { echo "WARNING: MPC did not quit within 5s; killing it." >&2; kill -9 $p; }
    fi
    for p in $(in_chroot); do
        case "$(readlink "/proc/$p/exe" 2>/dev/null)" in
            */root/touchbridge_mpc|*/crashpad_handler) kill "$p" 2>/dev/null ;;
        esac
    done
}

card_index() {
    local p="/dev/snd/by-path/$1"
    if [ -n "$1" ] && [ -L "$p" ]; then
        basename "$(readlink "$p")" | sed -n 's/^controlC\([0-9]*\)$/\1/p'
    else
        grep -E "^ *[0-9]+ \[${1:-<none>} *\]:" /proc/asound/cards | awk '{print $1; exit}'
    fi
}

start() {
    mountpoint -q "$R/proc" || up
    stop

    # MPC powers the device off, or reboots it, by running poweroff or reboot by
    # name once it has quit. Ahead of them on its PATH these note the request for
    # the loop below instead, so that the machine stays up.
    mkdir -p "$R/run/qengine-bin"
    for c in poweroff reboot; do
        printf '#!/bin/sh\necho %s > /run/qengine-power\n' "$c" > "$R/run/qengine-bin/$c"
        chmod 755 "$R/run/qengine-bin/$c"
    done
    rm -f "$R/run/qengine-power"
    # MPC leaks a few MB here per start (filmstrips).
    find "$R/var/volatile/tmp" -mindepth 1 -delete 2>/dev/null

    if [ -z "${AUDIO_CARD:-}" ]; then
        AUDIO_CARD="$(sed -n 's/^ *[0-9]* \[\([^] ]*\) *\]: USB-Audio.*/\1/p' /proc/asound/cards | head -n 1)"
        [ -n "$AUDIO_CARD" ] ||
            AUDIO_CARD="$(sed -n 's/^ *[0-9]* \[\([^] ]*\) *\]: .*/\1/p' /proc/asound/cards | grep -vx Dummy | head -n 1)"
    fi
    local out_index
    out_index="$(card_index "${AUDIO_CARD:-}")"
    [ -n "$out_index" ] ||
        echo "WARNING: no sound card '${AUDIO_CARD:-}'; MPC plays to snd-dummy only." >&2

    if [ -z "${MPC_TB_ARGS+x}" ]; then
        if grep -qE 'Name=.*(USB|Virtio) Tablet' /proc/bus/input/devices; then MPC_TB_ARGS=""
        else MPC_TB_ARGS="--mouse"; fi
    fi

    if ! in_chroot | while read -r p; do readlink "/proc/$p/exe"; done | grep -q '/dbus-daemon$'; then
        mkdir -p "$R/run/dbus"
        rm -f "$R/run/dbus/system_bus_socket" "$R/run/dbus/pid"
        chroot "$R" /usr/bin/env -i "${BASE[@]}" /usr/bin/dbus-daemon --system --fork
    fi

    # MPC looks for touchscreens once, as it starts.
    # shellcheck disable=SC2086  # MPC_TB_ARGS is a list of words on purpose
    setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /root/touchbridge_mpc $MPC_TB_ARGS \
        > "$R/run/touchbridge.log" 2>&1 < /dev/null &
    for _ in $(seq 1 50); do
        grep -q "TouchBridge Virtual Touchscreen" /proc/bus/input/devices && break; sleep 0.1
    done

    # As in a guest's acvs.service drop-in, with alsashim's second output on the
    # host's card in place of QEMU's. The launch is az01-launch-MPC's own, for
    # where systemd-inhibit is missing: there is no logind in here, so the host's
    # systemd-inhibit stands in and MPC gets the power key, which it reads itself
    # and answers with its own shutdown dialog. Shutting down from that dialog
    # quits MPC; its poweroff request is refused inside the chroot.
    local env=(LD_PRELOAD=/root/dtshim.so:/root/mpcsurface.so:/root/alsashim.so
               ALSASHIM_AS=off ALSASHIM_NOPLUG=1 ALSASHIM_BUFFER_SCALE=1 ALSASHIM_MIDI_CARD=-1
               MPCSURFACE_LOG=/run/mpcsurface.log
               GLIBC_TUNABLES=glibc.malloc.hugetlb=2 MALLOC_ARENA_MAX=1)
    [ -n "$out_index" ] && env+=("ALSASHIM_CUE_PCM=plughw:$out_index" ALSASHIM_CUE_CHANNEL=1)
    [ -n "${ALSASHIM_METER:-}" ] && env+=(ALSASHIM_METER=1)
    # Restarted whenever it quits, as acvs.service does (Restart=always): MPC
    # quits to switch between controller mode and standalone, and comes back up
    # in the other. Shutting down from MPC's own dialog ends the loop.
    # shellcheck disable=SC2016  # expanded by the shell in the chroot
    local SUPERVISOR='ulimit -S -s 1024
while :; do
    setarch -R -- /usr/bin/MPC
    echo "[chroot-mpc] MPC exited with $?"
    if [ "$(cat /run/qengine-power 2>/dev/null)" = poweroff ]; then
        echo "[chroot-mpc] MPC was shut down; not restarting it"; exit 0
    fi
    rm -f /run/qengine-power
    sleep 1
done'
    setsid systemd-inhibit --what=handle-power-key --mode=block --who=qengine \
        --why="MPC asks before shutting down" \
        chroot "$R" /usr/bin/env -i "${BASE[@]}" "${env[@]}" \
        PATH=/run/qengine-bin:/usr/sbin:/usr/bin:/sbin:/bin \
        /bin/sh -c "$SUPERVISOR" mpc-supervisor \
        > "$R/run/mpc.log" 2>&1 < /dev/null &

    echo "started $(cat "$R/root/fake-dt/inmusic,product-code" 2>/dev/null || echo MPC)," \
         "audio on ${AUDIO_CARD:-<none>}, log in $R/run/mpc.log"
}

down() {
    if mountpoint -q "$R"; then
        stop
        local pids
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
        # Not --make-rslave "$R" as a whole, unlike chroot-down.sh: services with
        # a private mount namespace (udevd, logind, resolved) got copies of these
        # mounts as they were made, and only a shared unmount reaches them; a
        # slave one leaves them holding the loop devices until the next boot.
        # /sys and /dev were made slaves in up, which is what keeps the host's.
        mount --make-rslave "$R/sys"
        mount --make-rslave "$R/dev"
        if ! umount -R "$R"; then
            echo "WARNING: could not unmount $R; still mounted:" >&2
            findmnt -R -n -o TARGET "$R" >&2
            exit 1
        fi
        echo "chroot at $R unmounted"
    fi
    losetup -j "$MPC_DIR/data_disk.img" 2>/dev/null | cut -d: -f1 | xargs -r -n 1 losetup -d
    systemctl start --no-block getty@tty1.service 2>/dev/null || true
}

case "${1:-start}" in
    up|start|stop|down) "${1:-start}" ;;
    *) echo "usage: $0 [up|start|stop|down]" >&2; exit 1 ;;
esac
