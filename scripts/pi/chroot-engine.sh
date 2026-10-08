#!/bin/bash
# chroot-engine.sh — launches Engine inside the chroot chroot-up.sh prepared, the
# way engine.service and its override would in a guest: D-Bus, the disk service,
# touchbridge and midisurface first, then the stock runengine script.
#
# Usage: sudo chroot-engine.sh
#   Safe to re-run; it stops the previous Engine first. Logs land in
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
#   SCREEN        "W H" to run at. Default "1280 800", the size the UI is laid
#                 out for; Engine has been seen to hang at start-up above it.
#   CONNECTOR     DRM connector to use, e.g. HDMI-A-2. Default: the first
#                 connected HDMI port.
#   AUDIO_CARD    sound card by ALSA id (the bracketed name in
#                 /proc/asound/cards), since card numbers follow plug order.
#                 Default: the first USB audio card. HDMI audio does not work.
#                 "Loopback" loads and uses ALSA's loopback card.
#   MIDI_FORWARD  a real USB controller to drive Engine with, as a substring of
#                 its ALSA sequencer name. Needs a mapping in the controllermap
#                 manifest for it and for this product. Default: none.
#   MIDI_FORWARD_ARGS  extra midisurface options that mapping calls for; its
#                 header lists them.
#   TB_ARGS, SHIMS_EXTRA, QT_LOGGING_RULES, ALSASHIM_*   passed through.
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

# /proc/asound/cards lines look like " 1 [Headset        ]: USB-Audio - ...".
card_id() { sed -n 's/^ *[0-9]* \[\([^] ]*\) *\]:.*/\1/p'; }
if [ -z "${AUDIO_CARD:-}" ]; then
    AUDIO_CARD="$(grep 'USB-Audio' /proc/asound/cards | card_id | head -n 1)"
fi
CARD_INDEX="$(grep -E "^ *[0-9]+ \[${AUDIO_CARD:-<none>} *\]:" /proc/asound/cards | awk '{print $1; exit}')"
if [ -z "$CARD_INDEX" ]; then
    echo "WARNING: no sound card with id '${AUDIO_CARD:-<none>}'; Engine will have no audio." >&2
    echo "         Available:" >&2; grep '^ *[0-9]' /proc/asound/cards >&2
    CARD_INDEX=0
fi

### environment ###############################################################
SHIMS=/root/cursorshim.so:/root/dtshim.so:/root/alsashim.so:/root/teeshim.so${SHIMS_EXTRA:+:$SHIMS_EXTRA}
BASE=(PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root)
ENVV=("${BASE[@]}"
      LD_PRELOAD=$SHIMS
      QT_QPA_PLATFORM=eglfs EGL_PLATFORM=gbm
      # The card name on this product's allowlist is its own product code, on
      # both products tried so far (RMZ2, JP24).
      ALSASHIM_CARD=$CARD_INDEX "ALSASHIM_AS=${ALSASHIM_AS:-$PRODUCT}"
      # A USB device's inputs are not ones Engine can start, and an input that
      # fails to start takes playback down with it.
      "ALSASHIM_NO_CAPTURE=${ALSASHIM_NO_CAPTURE-1}"
      "ALSASHIM_BUFFER_SCALE=${ALSASHIM_BUFFER_SCALE:-1}")
[ -n "${QT_LOGGING_RULES:-}" ] && ENVV+=("QT_LOGGING_RULES=$QT_LOGGING_RULES")
[ -n "${ALSASHIM_DEBUG:-}" ] && ENVV+=("ALSASHIM_DEBUG=1")
[ -n "${ALSASHIM_MAX_CHANNELS:-}" ] && ENVV+=("ALSASHIM_MAX_CHANNELS=$ALSASHIM_MAX_CHANNELS")
[ -n "${CURSORSHIM_DEBUG:-}" ] && ENVV+=("CURSORSHIM_DEBUG=1")

### launch ####################################################################
# Anchored patterns: an unanchored one also matches the shell that invoked us,
# which over ssh is the session itself.
pkill -f '^/bin/sh /usr/Engine/Scripts/'
pkill -f '^/usr/Engine/Engine'; sleep 2; pkill -9 -f '^/usr/Engine/Engine'
pkill -f '^/root/touchbridge'; pkill -f '^/root/midisurface'
sleep 1

# Engine aborts at start-up without a system bus. The rootfs's own daemon, so it
# sees Engine OS's bus policy and not the host's.
if [ ! -S "$R/run/dbus/system_bus_socket" ]; then
    mkdir -p "$R/run/dbus"
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
# shellcheck disable=SC2086  # TB_ARGS is a list of words on purpose
setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /root/touchbridge $TB_ARGS \
    > "$R/run/touchbridge.log" 2>&1 < /dev/null &
setsid chroot "$R" /usr/bin/env -i "${BASE[@]}" /bin/sh -c \
    "exec 3<>/run/midisurface.fifo; exec /root/midisurface --motor-off ${MIDI_FORWARD:+--forward $MIDI_FORWARD ${MIDI_FORWARD_ARGS:-}} < /run/midisurface.fifo" \
    > "$R/run/midisurface.log" 2>&1 &
sleep 2
setsid chroot "$R" /usr/bin/env -i "${ENVV[@]}" /usr/Engine/Scripts/runengine \
    > "$R/run/engine.log" 2>&1 < /dev/null &

echo "started $PRODUCT at $MODE on $CONNECTOR, audio on ${AUDIO_CARD:-<none>}${MIDI_FORWARD:+, forwarding $MIDI_FORWARD}"
