#!/bin/bash
# chroot-engine.sh — launches Engine inside the chroot chroot-up.sh prepared, the
# way engine.service and its override would in a guest: D-Bus, the disk service,
# touchbridge and midisurface first, then the stock runengine script.
#
# Usage: sudo chroot-engine.sh
#   Safe to re-run; it stops the previous Engine first (chroot-down.sh stops
#   it and everything else, for good). Logs land in
#   $ENGINE_ROOT/run/*.log. Drive the virtual control surface with
#     echo 'play left' | sudo tee $ENGINE_ROOT/run/midisurface.fifo
#
# What differs from the QEMU launch, and why:
#   - no MESA_LOADER_DRIVER_OVERRIDE: Mesa picks v3d/vc4 from the real device
#   - no drmatomic and no QT_QPA_EGLFS_KMS_ATOMIC=0: both work around virtio-gpu.
#     SHIMS_EXTRA=/root/drmatomic.so puts the shim back if eglfs fails.
#   - touch comes from ordinary mice (touchbridge --mouse) with a drawn pointer
#   - Engine's own audio buffering: the deeper ring alsashim adds by default is
#     for QEMU's emulated card and only adds latency on a real one
#
# Environment (or $QENGINE_DIR/pi.env, which both scripts read):
#   QENGINE_DIR, ENGINE_ROOT   as in chroot-up.sh
#   PLATFORM      eglfs (default): Engine owns the display, as on a unit.
#                 wayland: Engine is a window in a compositor already running
#                 on the host, such as sway. Needs the Wayland Qt and Mesa that
#                 stage_for_pi.sh --wayland stages. The compositor then owns the
#                 display, mouse and pointer, so CONNECTOR, TB_ARGS and
#                 cursorshim do not apply. SCREEN is the size Engine is told
#                 its screen is, and has to be the size of its window, since
#                 Engine lays out for the screen: the whole output when it is
#                 fullscreen (the default), or e.g. "1280 800" in a window that
#                 size. QT_SCALE_FACTOR applies to it as under eglfs.
#   WAYLAND_PATH  with PLATFORM=wayland, the compositor's socket on the host.
#                 Default: the newest /run/user/*/wayland-N.
#   SCREEN        "W H" to run at. Default "1280 800", the size the UI is laid
#                 out for. Engine hangs at start-up at any other height unless
#                 QT_SCALE_FACTOR is set to H / 800, so that it still lays out
#                 800 high: SCREEN="1920 1080" QT_SCALE_FACTOR=1.35.
#   CONNECTOR     DRM connector to use, e.g. HDMI-A-2. Default: the first
#                 connected HDMI port.
#   AUDIO_CARD    sound card by ALSA id (the bracketed name in
#                 /proc/asound/cards), since card numbers follow plug order.
#                 Default: the first USB audio card. The monitor's speakers
#                 are vc4hdmi0 (HDMI-A-1) or vc4hdmi1 (HDMI-A-2).
#                 "Loopback" loads and uses ALSA's loopback card. Or a name
#                 in /dev/snd/by-path, the USB socket the card is in, for two
#                 cards with the same id: a DDJ-FLX10's two USB ports are two
#                 identical cards, DDJFLX10 and DDJFLX10_1 in plug order.
#   CUE_CARD     a second sound card, by ALSA id, to play one more of Engine's
#                 outputs on -- headphones on a USB card while the master goes
#                 to HDMI, say. Default: none.
#   CUE_CHANNEL   which output that is: the first channel of its pair, counting
#                 from 1. Default 3. ALSASHIM_METER=1 logs every channel's level
#                 to engine.log, which shows what a product puts where.
#   CARD_WAIT     seconds to wait for AUDIO_CARD and CUE_CARD to appear before
#                 going on without them. Default 0; qengine.service sets it.
#   MIDI_FORWARD  a real USB controller to drive Engine with, as a substring of
#                 its ALSA sequencer name. Needs a mapping in the controllermap
#                 manifest for it and for this product. Default: none.
#   MIDI_FORWARD_ARGS  extra midisurface options that mapping calls for; its
#                 header lists them.
#   TB_ARGS, SHIMS_EXTRA, QT_LOGGING_RULES, QT_SCALE_FACTOR, ALSASHIM_*, MONTERO_*   passed through.
set -uo pipefail

QENGINE_DIR="${QENGINE_DIR:-$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/qengine}"
# Settings file first, then the caller's environment again on top of it, so that
# `sudo AUDIO_CARD=... chroot-engine.sh` overrides the file for one run.
_caller_env="$(export -p)"
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
eval "$_caller_env"
R="${ENGINE_ROOT:-/srv/engine}"

mountpoint -q "$R/proc" || { echo "ERROR: run chroot-up.sh first." >&2; exit 1; }

PRODUCT="$(cat "$R/root/fake-dt/inmusic,product-code" 2>/dev/null || true)"
[ -n "$PRODUCT" ] || { echo "ERROR: no product code in $R/root/fake-dt." >&2; exit 1; }

### display ###################################################################
PLATFORM="${PLATFORM:-eglfs}"
case "$PLATFORM" in eglfs|wayland) ;; *)
    echo "ERROR: PLATFORM is eglfs or wayland, not '$PLATFORM'." >&2; exit 1 ;;
esac

if [ "$PLATFORM" = wayland ]; then
    # The compositor runs on the host as its own user, and its socket lives in
    # that user's runtime directory, which the chroot cannot see. The directory
    # is bound in, not the socket, so a restarted compositor is still reachable.
    if [ -z "${WAYLAND_PATH:-}" ]; then
        WAYLAND_PATH="$(ls -t /run/user/*/wayland-[0-9]* 2>/dev/null | grep -v '\.lock$' | head -n 1)"
    fi
    [ -S "${WAYLAND_PATH:-}" ] || {
        echo "ERROR: no Wayland compositor socket${WAYLAND_PATH:+ at $WAYLAND_PATH}; start one or set WAYLAND_PATH." >&2
        exit 1; }
    [ -e "$R/usr/lib/plugins/platforms/libqwayland-egl.so" ] || {
        echo "ERROR: this rootfs has no Wayland Qt; stage with --wayland and reinstall." >&2; exit 1; }
    mkdir -p "$R/run/qengine-wayland"
    mountpoint -q "$R/run/qengine-wayland" && umount "$R/run/qengine-wayland"
    mount --bind "$(dirname "$WAYLAND_PATH")" "$R/run/qengine-wayland"
    # Qt wants a runtime directory of its own, private to the user it runs as.
    mkdir -p -m 0700 "$R/run/user/0"
else
    # The size has to agree in three places: the "mode" Qt is given, the touch
    # device and the pointer. touchbridge would otherwise size itself from the
    # monitor's preferred mode, which is not the one in use.
    SCREEN="${SCREEN:-1280 800}"
    MODE="${SCREEN/ /x}"

    if [ -z "${CONNECTOR:-}" ]; then
        for c in /sys/class/drm/card*-HDMI-A-*; do
            [ "$(cat "$c/status" 2>/dev/null)" = connected ] || continue
            CONNECTOR="${c##*/card?-}"
            break
        done
    fi
    [ -n "${CONNECTOR:-}" ] || {
        echo "ERROR: no connected HDMI output; plug a display in or set CONNECTOR." >&2; exit 1; }

    # Rewritten from the vendor file on every launch, so a changed port or size
    # takes effect and nothing accumulates. Three things the stock file cannot know:
    #   device  a Pi has two DRM devices -- v3d renders, vc4 owns the connectors --
    #           and Qt has to be pointed at the one with a display on it. By path,
    #           because their card numbers are not stable across boots.
    #   name    Qt ignores settings for an output it cannot match, and names
    #           connectors without punctuation (HDMI-A-2 is "HDMI2").
    #   mode    otherwise Qt takes the monitor's preferred mode.
    SCREEN_CFG="$R/usr/Engine/ScreenConfiguration/$PRODUCT/ScreenConfiguration.json"
    [ -f "$SCREEN_CFG" ] || {
        echo "ERROR: this firmware has no screen configuration for $PRODUCT." >&2; exit 1; }
    [ -f "$SCREEN_CFG.stock" ] || cp -a "$SCREEN_CFG" "$SCREEN_CFG.stock"
    KMS_DEVICE="$(ls /dev/dri/by-path/*gpu-card 2>/dev/null | head -n 1)"
    python3 - "$SCREEN_CFG" "${CONNECTOR/-A-/}" "$MODE" "$KMS_DEVICE" <<'EOF'
import json, sys
path, name, mode, device = sys.argv[1:5]
cfg = json.load(open(path + ".stock"))
if device:
    cfg["device"] = device
out = cfg["outputs"][0]
out["name"] = name
out["mode"] = mode
cfg["outputs"] = [out]
json.dump(cfg, open(path, "w"), indent=4)
EOF

    TB_ARGS="${TB_ARGS:---pointer --mouse --symlink /dev/input/qengine-touch0 $SCREEN}"
fi

### audio #####################################################################
# AUDIO_CARD=Loopback sends Engine's output to ALSA's loopback card, for
# listening from another machine; README.md has the command for the other end.
#
# It needs the deeper ring a real card does not: the loopback card keeps time
# with kernel timer ticks (4ms on a Pi), which is too coarse for Engine's own
# 12ms buffer -- the stream underran and restarted about 75 times a second, and
# the decks visibly ran slow. Latency does not matter on this path anyway.
if [ "${AUDIO_CARD:-}" = Loopback ]; then
    modprobe snd-aloop 2>/dev/null
    ALSASHIM_BUFFER_SCALE="${ALSASHIM_BUFFER_SCALE:-8}"
fi

# A card's index: by its ALSA id, or by its name in /dev/snd/by-path.
# /proc/asound/cards lines look like " 1 [Headset        ]: USB-Audio - ...".
card_index() {
    local p="/dev/snd/by-path/$1"
    if [ -n "$1" ] && [ -L "$p" ]; then
        basename "$(readlink "$p")" | sed -n 's/^controlC\([0-9]*\)$/\1/p'
    else
        grep -E "^ *[0-9]+ \[${1:-<none>} *\]:" /proc/asound/cards | awk '{print $1; exit}'
    fi
}

# Started at boot (qengine.service), this can run before a USB card has
# enumerated: wait up to CARD_WAIT seconds for the ones named.
for _ in $(seq 1 "${CARD_WAIT:-0}"); do
    missing=""
    for c in "${AUDIO_CARD:-}" "${CUE_CARD:-}"; do
        [ -z "$c" ] || [ -n "$(card_index "$c")" ] || missing=1
    done
    [ -n "$missing" ] || break
    sleep 1
done

card_id() { sed -n 's/^ *[0-9]* \[\([^] ]*\) *\]:.*/\1/p'; }
if [ -z "${AUDIO_CARD:-}" ]; then
    AUDIO_CARD="$(grep 'USB-Audio' /proc/asound/cards | card_id | head -n 1)"
fi
CARD_INDEX="$(card_index "${AUDIO_CARD:-}")"
if [ -z "$CARD_INDEX" ]; then
    echo "WARNING: no sound card with id '${AUDIO_CARD:-<none>}'; Engine will have no audio." >&2
    echo "         Available:" >&2; grep '^ *[0-9]' /proc/asound/cards >&2
    CARD_INDEX=0
fi
CUE_INDEX=""
if [ -n "${CUE_CARD:-}" ]; then
    CUE_INDEX="$(card_index "$CUE_CARD")"
    [ -n "$CUE_INDEX" ] ||
        echo "WARNING: no sound card with id '$CUE_CARD'; no second output." >&2
fi

### environment ###############################################################
# Under a compositor the pointer is the compositor's to draw.
SHIMS=/root/dtshim.so:/root/alsashim.so:/root/teeshim.so:/root/quitshim.so${SHIMS_EXTRA:+:$SHIMS_EXTRA}
[ "$PLATFORM" = eglfs ] && SHIMS=/root/cursorshim.so:$SHIMS
BASE=(PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root)
if [ "$PLATFORM" = wayland ]; then
    # No decorations: the window is the whole interface, and the compositor
    # can draw a frame if it wants one. Engine ignores mouse clicks, so the
    # Wayland plugin hands the left button over as touch (what touchbridge does
    # under eglfs) -- an option of this build of it, see build_qtwayland.sh.
    QPA=(QT_QPA_PLATFORM=wayland-egl XDG_RUNTIME_DIR=/run/user/0
         "WAYLAND_DISPLAY=/run/qengine-wayland/$(basename "$WAYLAND_PATH")"
         QT_WAYLAND_DISABLE_WINDOWDECORATION=1 QT_WAYLAND_MOUSE_AS_TOUCH=1)
    # Also this build's: the screen reported as the window's size.
    [ -n "${SCREEN:-}" ] && QPA+=("QT_WAYLAND_SCREEN_SIZE=${SCREEN/ /x}")
else
    QPA=(QT_QPA_PLATFORM=eglfs EGL_PLATFORM=gbm)
fi
ENVV=("${BASE[@]}"
      LD_PRELOAD=$SHIMS
      "${QPA[@]}"
      # The card name on this product's allowlist is its own product code, on
      # both products tried so far (RMZ2, JP24).
      ALSASHIM_CARD=$CARD_INDEX "ALSASHIM_AS=${ALSASHIM_AS:-$PRODUCT}"
      # A USB device's inputs are not ones Engine can start, and an input that
      # fails to start takes playback down with it.
      "ALSASHIM_NO_CAPTURE=${ALSASHIM_NO_CAPTURE-1}"
      "ALSASHIM_BUFFER_SCALE=${ALSASHIM_BUFFER_SCALE:-1}")
[ -n "${QT_LOGGING_RULES:-}" ] && ENVV+=("QT_LOGGING_RULES=$QT_LOGGING_RULES")
[ -n "${QT_SCALE_FACTOR:-}" ] && ENVV+=("QT_SCALE_FACTOR=$QT_SCALE_FACTOR")
[ -n "$CUE_INDEX" ] && ENVV+=("ALSASHIM_CUE_PCM=plughw:$CUE_INDEX" "ALSASHIM_CUE_CHANNEL=${CUE_CHANNEL:-3}")
[ -n "${ALSASHIM_DEBUG:-}" ] && ENVV+=("ALSASHIM_DEBUG=1")
[ -n "${ALSASHIM_METER:-}" ] && ENVV+=("ALSASHIM_METER=1")
[ -n "${ALSASHIM_MAX_CHANNELS:-}" ] && ENVV+=("ALSASHIM_MAX_CHANNELS=$ALSASHIM_MAX_CHANNELS")
[ -n "${CURSORSHIM_DEBUG:-}" ] && ENVV+=("CURSORSHIM_DEBUG=1")
[ -n "${MONTERO_DUMP:-}" ] && ENVV+=("MONTERO_DUMP=1")
[ -n "${MONTERO_EXTRACT:-}" ] && ENVV+=("MONTERO_EXTRACT=$MONTERO_EXTRACT")
[ -n "${MONTERO_LOG_URLS:-}" ] && ENVV+=("MONTERO_LOG_URLS=1")
[ -n "${MONTERO_MODS:-}" ] && ENVV+=("MONTERO_MODS=$MONTERO_MODS")

### launch ####################################################################
# Anchored patterns: an unanchored one also matches the shell that invoked us,
# which over ssh is the session itself.
# powerkey.py first: it acts on how Engine exits.
pkill -f '/powerkey\.py '
pkill -f '^/bin/sh /usr/Engine/Scripts/'
# With quitshim preloaded SIGTERM asks Engine to quit, which takes it a moment.
pkill -f '^/usr/Engine/Engine'
for _ in $(seq 1 200); do pgrep -f '^/usr/Engine/Engine' >/dev/null || break; sleep 0.1; done
pkill -9 -f '^/usr/Engine/Engine'
pkill -f '^/root/touchbridge'; pkill -f '^/root/midisurface'
sleep 1

# Engine copies every track it loads, and that track's stems, into this directory
# and works from the copy. Nothing in this setup ever removes them -- they were
# found at 270 files and 900MB after a day, 25 copies of each sampler sound among
# them, one set per launch -- and /data is a small image. Engine is stopped at
# this point, so nothing here is in use.
if [ -d "$R/data/inMusicTempFiles" ]; then
    find "$R/data/inMusicTempFiles" -mindepth 1 -delete
fi

# Engine aborts at start-up without a system bus. The rootfs's own daemon, so it
# sees Engine OS's bus policy and not the host's. Asked of the process and not
# the socket: stopping qengine.service kills the daemon and leaves its socket
# and pid file in the chroot's /run, and it will not start over either.
if ! pgrep -f '^/usr/bin/dbus-daemon --system --fork$' >/dev/null; then
    mkdir -p "$R/run/dbus"
    rm -f "$R/run/dbus/system_bus_socket" "$R/run/dbus/pid"
    chroot "$R" /usr/bin/env -i "${BASE[@]}" /usr/bin/dbus-daemon --system --fork
fi

# Engine OS's disk service: what mounts USB drives and SD cards under /media and
# tells Engine about them. On the real system D-Bus starts it through systemd;
# here it is started by hand, once, and left running across Engine restarts.
# Engine only looks for it at start-up, so it has to come first.
if ! pgrep -f '^/usr/libexec/edisksd' >/dev/null; then
    setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /usr/libexec/edisksd \
        > "$R/run/edisksd.log" 2>&1 < /dev/null &
    sleep 1
fi

# Before the surface and Engine, both of which only read it at start-up. The
# rootfs is already writable here and must stay so, hence no remounting.
if [ -x "$R/root/controllermap/controllermap.sh" ]; then
    chroot "$R" /usr/bin/env -i "${BASE[@]}" CONTROLLERMAP_NO_REMOUNT=1 \
        /root/controllermap/controllermap.sh > "$R/run/controllermap.log" 2>&1 || true
fi

rm -f "$R/run/midisurface.fifo"; mkfifo "$R/run/midisurface.fifo"
if [ "$PLATFORM" = eglfs ]; then
    # shellcheck disable=SC2086  # TB_ARGS is a list of words on purpose
    setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /root/touchbridge $TB_ARGS \
        > "$R/run/touchbridge.log" 2>&1 < /dev/null &
fi
setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /bin/sh -c \
    "exec 3<>/run/midisurface.fifo; exec /root/midisurface --motor-off ${MIDI_FORWARD:+--forward $MIDI_FORWARD ${MIDI_FORWARD_ARGS:-}} < /run/midisurface.fifo" \
    > "$R/run/midisurface.log" 2>&1 &
sleep 2
setsid chroot "$R" /usr/bin/env -i "${ENVV[@]}" /usr/Engine/Scripts/runengine \
    > "$R/run/engine.log" 2>&1 < /dev/null &

# The power button, and a Yes to Engine's own "Turn Off" prompt: see powerkey.py.
# On the host, since powering it off is not something the chroot can do. The
# lock keeps the login manager from powering off on the first press, and is
# held for as long as powerkey.py runs.
if [ -x "$QENGINE_DIR/powerkey.py" ]; then
    setsid systemd-inhibit --what=handle-power-key --mode=block --who=qengine \
        --why="Engine asks before turning off" "$QENGINE_DIR/powerkey.py" "$R" \
        > "$R/run/powerkey.log" 2>&1 < /dev/null &
fi

if [ "$PLATFORM" = wayland ]; then
    WHERE="in the compositor at $WAYLAND_PATH"
else
    WHERE="at $MODE on $CONNECTOR"
fi
echo "started $PRODUCT $WHERE, audio on ${AUDIO_CARD:-<none>}${CUE_INDEX:+ and $CUE_CARD}${MIDI_FORWARD:+, forwarding $MIDI_FORWARD}"
