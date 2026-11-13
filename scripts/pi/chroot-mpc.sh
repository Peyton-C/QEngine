#!/bin/bash
# chroot-mpc.sh — runs a Gen 2 MPC, MPC Sample or HeadRush VX5 instance's rootfs
# in a chroot on an arm64 Linux host, the way chroot-up.sh and chroot-engine.sh
# do for Engine.
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
#   MPC_TB_ARGS  touchbridge options. Default: none with a QEMU tablet (a VM),
#                `--mouse` otherwise, which turns a USB mouse into the touchscreen.
#   PLATFORM     `wayland` to run MPC as a window in a running compositor (sway)
#                instead of on the display: kmsshim.so is preloaded into MPC as
#                its display, and kmspresent shows it and is its touchscreen. Both
#                are looked for in $QENGINE_DIR. Default: MPC drives the display.
#   WAYLAND_PATH the compositor's socket. Default: the newest in /run/user/*/.
#   ALSASHIM_METER=1  log the output's peak level in mpc.log every second.
#
# Logs are in $MPC_ROOT/run/: mpc.log, touchbridge.log or kmspresent.log,
# mpcsurface.log. The panel takes commands on $MPC_ROOT/run/mpcsurface.fifo, as
# in a guest.
#
# Engine and MPC both drive the display directly, so without PLATFORM=wayland
# run one at a time.
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

# By command line: MPC names its main thread "MPC 3 Main". Any of the apps.
mpc_pid() {
    local p
    for p in $(pgrep -f '^/usr/bin/(MPC|HV03)'); do
        [ "$(readlink "/proc/$p/root" 2>/dev/null)" = "$R" ] && echo "$p"
    done
}

# Which app this rootfs holds, and what differs between them. Both are inMusic's
# JUCE codebase, drawing to the display themselves and opening their internal
# card as /dev/snd/by-path/platform-sound, so the rest of this script is shared.
#   MPC (Gen 2)      1280x800 touchscreen; the stock snd-dummy card.
#   MPC Sample 1.3.0 320x240, no touchscreen, its panel only. It opens its card
#                    as four channels of S32_LE at 44.1 kHz, which only the
#                    qengine-ac50 model get_kernel.sh adds to snd-dummy offers.
#   HeadRush VX5 1.2.2  160x128, no touchscreen. Its panel is input devices at
#                    fixed paths, which vx5surface makes; its card is stereo
#                    S32_LE at 48 kHz, which qengine-ac50 also offers.
# The preloads and environment are each app's QEngine unit drop-in's, and the
# launch its launcher's (MPC's and the Sample's), or the bare binary (VX5's).
app_detect() {
    APP_PANEL=""; APP_WRAP=1
    if [ -x "$R/usr/bin/MPC" ]; then
        APP_BIN=/usr/bin/MPC; APP_MODEL=""; APP_SCREEN=1280x800; APP_TOUCH=1
        APP_ID=qengine-mpc; APP_NAME=MPC
        APP_PRELOAD=/root/dtshim.so:/root/mpcsurface.so:/root/alsashim.so
        APP_ENV=(GLIBC_TUNABLES=glibc.malloc.hugetlb=2 MALLOC_ARENA_MAX=1)
    elif [ -x "$R/usr/bin/MPC Sample" ]; then
        APP_BIN="/usr/bin/MPC Sample"; APP_MODEL=qengine-ac50; APP_SCREEN=320x240; APP_TOUCH=0
        APP_ID=qengine-mpc-sample; APP_NAME="MPC Sample"
        APP_PRELOAD=/root/dtshim.so:/root/mpcsurface.so:/root/alsashim.so
        APP_ENV=(MALLOC_ARENA_MAX=1)
    elif [ -x "$R/usr/bin/HV03" ]; then
        APP_BIN=/usr/bin/HV03; APP_MODEL=qengine-ac50; APP_SCREEN=160x128; APP_TOUCH=0
        APP_ID=qengine-vx5; APP_NAME="HeadRush VX5"; APP_PANEL=vx5surface; APP_WRAP=0
        APP_PRELOAD=/root/dtshim.so:/root/alsashim.so
        # 1.2.2's DSP worker can start with a garbage sample count; zeroed fresh
        # allocations, tcache's included, make it start with none (build_mpc_rootfs.sh).
        APP_ENV=(ALSASHIM_INPUT_WAV=/data/input.wav MALLOC_PERTURB_=255
                 GLIBC_TUNABLES=glibc.malloc.tcache_count=0)
    else
        APP_BIN=""
    fi
    # Several of these can run at once, each in its own MPC_ROOT. Each gets a
    # seat of its own for its touchscreen and panel, so that one app's window
    # is not another's input. DUMMY_MODEL puts them all on one snd-dummy card,
    # which has substreams to spare: "default" for the stock card.
    APP_SEAT="seat-$APP_ID"
    APP_TOUCH_NAME="QEngine kmspresent $APP_ID"
    case "${DUMMY_MODEL:-}" in
        "") ;;
        default) APP_MODEL="" ;;
        *) APP_MODEL="$DUMMY_MODEL" ;;
    esac
}

up() {
    [ -f "$MPC_DIR/rootfs.img" ] && [ -f "$MPC_DIR/data_disk.img" ] || {
        echo "ERROR: no rootfs.img and data_disk.img in $MPC_DIR (set MPC_DIR)." >&2; exit 1; }
    mkdir -p "$R"
    # The mount point is bound onto itself and made private before anything goes
    # on it. Services that run in a mount namespace of their own (logind, udevd,
    # resolved, colord, ...) otherwise get a copy of every mount made here, which
    # no unmount on the host reaches once /sys and /dev are slaves, and which keeps
    # the loop devices busy until the next boot. Only the empty bind propagates.
    if ! mountpoint -q "$R"; then
        mount --bind "$R" "$R" && mount --make-private "$R"
    fi
    # Read-only, as on the device. Everything MPC writes goes to /data or a tmpfs.
    findmnt -n -o SOURCE --mountpoint "$R" | grep -q '^/dev/loop' ||
        mount -o loop,ro "$MPC_DIR/rootfs.img" "$R" || exit 1
    app_detect
    [ -n "$APP_BIN" ] && [ -e "$R/lib/ld-linux-aarch64.so.1" ] || {
        echo "ERROR: $MPC_DIR/rootfs.img is not an arm64 MPC, MPC Sample or VX5 rootfs." >&2
        umount "$R"; exit 1; }

    # data_disk.img is /data and /factory, the two partitions new_instance.sh
    # gave it. The vendor mount units find them by PARTUUID, which the host's
    # loop devices also carry, but there is no systemd in here to read the units.
    local loop
    loop="$(losetup -j "$MPC_DIR/data_disk.img" | cut -d: -f1 | head -n 1)"
    [ -n "$loop" ] || loop="$(losetup -fP --show "$MPC_DIR/data_disk.img")"
    mountpoint -q "$R/data"    || mount "${loop}p1" "$R/data"
    # The VX5 rootfs has no /factory to mount it on.
    [ ! -d "$R/factory" ] || mountpoint -q "$R/factory" || mount "${loop}p2" "$R/factory"

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
    # The model is chosen when the module loads, so a card loaded for the other
    # app is reloaded, which only works while nothing has it open.
    local loaded
    loaded="$(cut -d, -f1 /sys/module/snd_dummy/parameters/model 2>/dev/null)"
    [ "$loaded" = "(null)" ] && loaded=""
    if [ -d /sys/module/snd_dummy ] && [ "$loaded" != "$APP_MODEL" ]; then
        modprobe -r snd-dummy || {
            echo "ERROR: snd-dummy is loaded as '${loaded:-default}' and in use; this app needs '${APP_MODEL:-default}'." >&2
            exit 1; }
    fi
    modprobe snd-dummy ${APP_MODEL:+model=$APP_MODEL} || {
        echo "ERROR: this kernel has no snd-dummy." >&2; exit 1; }
    # An unknown model is ignored by snd-dummy, which then loads its own card.
    # Distribution modules are compressed, and have no such model anyway.
    if [ -n "$APP_MODEL" ] && ! grep -qa "$APP_MODEL" "$(modinfo -n snd-dummy)" 2>/dev/null; then
        echo "WARNING: this snd-dummy has no $APP_MODEL model, and the app will find no usable card." >&2
        echo "         Build it as scripts/build_scripts/get_kernel.sh does, against this kernel." >&2
    fi
    local rule=/etc/udev/rules.d/90-qengine-mpc-audio.rules
    local want='SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{id}=="Dummy", SYMLINK+="snd/by-path/platform-sound"'
    if [ "$(cat "$rule" 2>/dev/null)" != "$want" ]; then
        printf '%s\n' "$want" > "$rule"
        udevadm control --reload
        udevadm trigger --subsystem-match=sound --action=change
    fi
    # The link appears once udev has handled the card modprobe just added, which
    # it does after modprobe returns.
    udevadm settle
    [ -e /dev/snd/by-path/platform-sound ] || {
        echo "ERROR: snd-dummy loaded but /dev/snd/by-path/platform-sound is missing." >&2; exit 1; }
    # A desktop's PipeWire opens every card it finds, MPC's included, and MPC
    # then cannot. WirePlumber reads this when it next starts.
    local wp=/etc/wireplumber/wireplumber.conf.d/90-qengine-mpc.conf
    local wpwant='# Written by qengine chroot-mpc.sh: snd-dummy is MPC'"'"'s internal card, not the desktop'"'"'s.
monitor.alsa.rules = [
  {
    matches = [ { device.name = "~alsa_card.platform-snd_dummy*" } ]
    actions = { update-props = { device.disabled = true } }
  }
]'
    if [ -d /etc/wireplumber ] || command -v wireplumber >/dev/null; then
        if [ "$(cat "$wp" 2>/dev/null)" != "$wpwant" ]; then
            mkdir -p "$(dirname "$wp")"; printf '%s\n' "$wpwant" > "$wp"
            echo "NOTE: wrote $wp; restart WirePlumber in any desktop session that is running." >&2
        fi
    fi

    # The login prompt on tty1 shares the display MPC renders to. Not under a
    # compositor, which has the display itself, and only stopped if running,
    # so that down knows whether to bring it back: a display manager conflicts
    # with it, and starting the getty would stop the display manager and end
    # its sessions.
    if [ "${PLATFORM:-}" != wayland ] && systemctl -q is-active getty@tty1.service; then
        systemctl stop getty@tty1.service && touch /run/qengine-mpc-getty
    fi
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
            */root/touchbridge_mpc|*/root/vx5surface|*/crashpad_handler) kill "$p" 2>/dev/null ;;
        esac
    done
    # kmspresent runs on the host, so it is found by the pid it left.
    if [ -f "$R/run/kmspresent.pid" ]; then
        p="$(cat "$R/run/kmspresent.pid")"
        # " (deleted)" when kmspresent has been replaced since it started.
        case "$(readlink "/proc/$p/exe" 2>/dev/null)" in
            "$QENGINE_DIR/kmspresent"|"$QENGINE_DIR/kmspresent (deleted)") kill "$p" ;;
        esac
        rm -f "$R/run/kmspresent.pid"
    fi
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
    app_detect
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

    # The VX5's panel, phantom-power key and volume knob, as uinput devices at
    # the paths HV03 opens, and its f_midi port. The links are the guest's udev
    # rule, on the host. On kmspresent's seat, so that a desktop does not take
    # the panel's keys for a keyboard of its own.
    if [ "$APP_PANEL" = vx5surface ]; then
        local vrule=/etc/udev/rules.d/90-qengine-vx5-panel.rules
        local vwant="SUBSYSTEM==\"input\", KERNEL==\"event*\", ATTRS{name}==\"QEngine VX5 panel\", SYMLINK+=\"input/by-path/platform-fe620000.spi-cs-0-event\", ENV{ID_SEAT}=\"$APP_SEAT\"
SUBSYSTEM==\"input\", KERNEL==\"event*\", ATTRS{name}==\"QEngine VX5 volume\", SYMLINK+=\"input/by-path/platform-adc-joystick-event\", ENV{ID_SEAT}=\"$APP_SEAT\"
SUBSYSTEM==\"input\", KERNEL==\"event*\", ATTRS{name}==\"QEngine VX5 phantom power\", SYMLINK+=\"input/by-path/platform-gpio-keys-event\", OPTIONS+=\"link_priority=10\", ENV{ID_SEAT}=\"$APP_SEAT\""
        if [ "$(cat "$vrule" 2>/dev/null)" != "$vwant" ]; then
            printf '%s\n' "$vwant" > "$vrule"
            udevadm control --reload
        fi
        rm -f "$R/run/vx5surface.fifo"; mkfifo "$R/run/vx5surface.fifo"
        setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /bin/sh -c \
            'exec 3<>/run/vx5surface.fifo; exec /root/vx5surface < /run/vx5surface.fifo' \
            > "$R/run/vx5surface.log" 2>&1 &
        for _ in $(seq 1 50); do
            [ -e /dev/input/by-path/platform-fe620000.spi-cs-0-event ] &&
            [ -e /dev/input/by-path/platform-adc-joystick-event ] && break
            sleep 0.1
        done
        udevadm settle
    fi

    local platform="${PLATFORM:-kms}" display_shim="" seat_env=""
    # MPC looks for touchscreens once, as it starts, so whichever one it gets
    # has to exist first.
    if [ "$platform" = wayland ]; then
        [ -x "$QENGINE_DIR/kmspresent" ] && [ -f "$QENGINE_DIR/kmsshim.so" ] || {
            echo "ERROR: PLATFORM=wayland needs kmspresent and kmsshim.so in $QENGINE_DIR." >&2; exit 1; }
        if [ -z "${WAYLAND_PATH:-}" ]; then
            # Newest first; the names are wayland-N, nothing ls could mangle.
            # shellcheck disable=SC2010
            WAYLAND_PATH="$(ls -t /run/user/*/wayland-[0-9]* 2>/dev/null | grep -v '\.lock$' | head -n 1)"
        fi
        [ -S "${WAYLAND_PATH:-}" ] || {
            echo "ERROR: no Wayland compositor socket${WAYLAND_PATH:+ at $WAYLAND_PATH}; start one or set WAYLAND_PATH." >&2
            exit 1; }
        # The touchscreen kmspresent makes is MPC's, not the compositor's: a
        # compositor that took it too would feed every touch back to the window.
        # On a seat of its own the compositor leaves it alone, and kmsshim shows
        # MPC that seat as its seat0 (KMSSHIM_SEAT), and nothing else. Not
        # LIBINPUT_IGNORE_DEVICE: MPC reads input through libinput too.
        local rule="/etc/udev/rules.d/90-qengine-kmspresent-$APP_ID.rules"
        local want="SUBSYSTEM==\"input\", ATTRS{name}==\"$APP_TOUCH_NAME\", ENV{ID_SEAT}=\"$APP_SEAT\""
        rm -f /etc/udev/rules.d/90-qengine-kmspresent.rules  # one seat for all, before
        if [ "$(cat "$rule" 2>/dev/null)" != "$want" ]; then
            printf '%s\n' "$want" > "$rule"
            udevadm control --reload
        fi
        # A desktop names a window after the .desktop file matching its app id,
        # and otherwise shows the bare id. Hidden from the app grid: launching
        # from there is not how this starts.
        local desktop="/usr/local/share/applications/$APP_ID.desktop"
        local dwant
        dwant="$(printf '[Desktop Entry]\nType=Application\nName=%s\nComment=%s in a QEngine chroot\nIcon=audio-x-generic\nExec=/bin/true\nNoDisplay=true\n' "$APP_NAME" "$APP_NAME")"
        if [ "$(cat "$desktop" 2>/dev/null)" != "$dwant" ]; then
            mkdir -p "$(dirname "$desktop")"; printf '%s\n' "$dwant" > "$desktop"
        fi
        # In /run and not in the rootfs: nothing written for a session goes there.
        cp "$QENGINE_DIR/kmsshim.so" "$R/run/qengine-bin/kmsshim.so"
        display_shim=:/run/qengine-bin/kmsshim.so
        seat_env="KMSSHIM_SEAT=$APP_SEAT"
        setsid env XDG_RUNTIME_DIR="$(dirname "$WAYLAND_PATH")" WAYLAND_DISPLAY="$(basename "$WAYLAND_PATH")" \
            "$QENGINE_DIR/kmspresent" --socket "$R/run/kmsshim.sock" --size "$APP_SCREEN" \
            --title "$APP_NAME" --app-id "$APP_ID" --name "$APP_TOUCH_NAME" \
            > "$R/run/kmspresent.log" 2>&1 < /dev/null &
        echo $! > "$R/run/kmspresent.pid"
        for _ in $(seq 1 50); do
            grep -qF "Name=\"$APP_TOUCH_NAME\"" /proc/bus/input/devices && break; sleep 0.1
        done
        grep -qF "Name=\"$APP_TOUCH_NAME\"" /proc/bus/input/devices || {
            echo "ERROR: kmspresent did not start; see $R/run/kmspresent.log" >&2; exit 1; }
    elif [ "$APP_TOUCH" = 1 ]; then
        # shellcheck disable=SC2086  # MPC_TB_ARGS is a list of words on purpose
        setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /root/touchbridge_mpc $MPC_TB_ARGS \
            > "$R/run/touchbridge.log" 2>&1 < /dev/null &
        for _ in $(seq 1 50); do
            grep -q "TouchBridge Virtual Touchscreen" /proc/bus/input/devices && break; sleep 0.1
        done
    fi

    # As in a guest's acvs.service drop-in, with alsashim's second output on the
    # host's card in place of QEMU's. The launch is az01-launch-MPC's own, for
    # where systemd-inhibit is missing: there is no logind in here, so the host's
    # systemd-inhibit stands in and MPC gets the power key, which it reads itself
    # and answers with its own shutdown dialog. Shutting down from that dialog
    # quits MPC; its poweroff request is refused inside the chroot.
    local env=("LD_PRELOAD=$APP_PRELOAD$display_shim"
               KMSSHIM_LOG=/run/kmsshim.log "KMSSHIM_MODE=$APP_SCREEN" "QENGINE_APP=$APP_BIN"
               ALSASHIM_AS=off ALSASHIM_NOPLUG=1 ALSASHIM_BUFFER_SCALE=1 ALSASHIM_MIDI_CARD=-1
               MPCSURFACE_LOG=/run/mpcsurface.log "QENGINE_WRAP=$APP_WRAP" "${APP_ENV[@]}")
    [ -n "$out_index" ] && env+=("ALSASHIM_CUE_PCM=plughw:$out_index" ALSASHIM_CUE_CHANNEL=1)
    [ -n "${ALSASHIM_METER:-}" ] && env+=(ALSASHIM_METER=1)
    [ -n "$seat_env" ] && env+=("$seat_env")
    # Restarted whenever it quits, as acvs.service does (Restart=always): MPC
    # quits to switch between controller mode and standalone, and comes back up
    # in the other. Shutting down from MPC's own dialog ends the loop.
    # shellcheck disable=SC2016  # expanded by the shell in the chroot
    local SUPERVISOR='[ "$QENGINE_WRAP" = 1 ] && ulimit -S -s 1024
while :; do
    if [ "$QENGINE_WRAP" = 1 ]; then setarch -R -- "$QENGINE_APP"; else "$QENGINE_APP"; fi
    echo "[chroot-mpc] $QENGINE_APP exited with $?"
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

    echo "started $(cat "$R/root/fake-dt/inmusic,product-code" 2>/dev/null || echo MPC)" \
         "${display_shim:+in the compositor at $WAYLAND_PATH, }audio on ${AUDIO_CARD:-<none>}," \
         "log in $R/run/mpc.log"
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
        # /sys and /dev were made slaves in up, which is what keeps the host's
        # own submounts of them mounted. Twice: the rootfs, then the private
        # bind under it.
        mount --make-rslave "$R/sys" 2>/dev/null
        mount --make-rslave "$R/dev" 2>/dev/null
        if ! { umount -R "$R" && { ! mountpoint -q "$R" || umount "$R"; }; }; then
            echo "WARNING: could not unmount $R; still mounted:" >&2
            findmnt -R -n -o TARGET "$R" >&2
            exit 1
        fi
        echo "chroot at $R unmounted"
    fi
    losetup -j "$MPC_DIR/data_disk.img" 2>/dev/null | cut -d: -f1 | xargs -r -n 1 losetup -d
    if [ -e /run/qengine-mpc-getty ]; then
        rm -f /run/qengine-mpc-getty
        systemctl start --no-block getty@tty1.service 2>/dev/null || true
    fi
}

case "${1:-start}" in
    up|start|stop|down) "${1:-start}" ;;
    *) echo "usage: $0 [up|start|stop|down]" >&2; exit 1 ;;
esac
