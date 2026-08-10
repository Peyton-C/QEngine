#!/bin/sh
# run-prime4.sh — launch Engine spoofed as Prime 4 (JC11), with VNC output
# and real mouse-to-touch input via vnctouchbridge.
#
# Run this from inside the guest, after boot, as root. Safe to re-run without
# rebooting — kills any previous bridge/Engine instances first.
#
# Prime 4 has no battery, so unlike Prime Go there's no "touch and hold the
# logo" gate or auto-quit timer — Engine just starts straight up.

PRODUCT_CODE=JC11
VNC_SIZE=1280x720
BRIDGE_PORT=5902
UPSTREAM_PORT=5900

echo "=== Setting up environment ==="
mount -o remount,rw /
systemctl stop engine.service 2>/dev/null

insmod /root/snd-seq-device.ko 2>/dev/null
insmod /root/snd-hwdep.ko 2>/dev/null
insmod /root/snd-rawmidi.ko 2>/dev/null
insmod /root/snd-seq.ko 2>/dev/null
insmod /root/snd-seq-midi-event.ko 2>/dev/null
insmod /root/snd-seq-midi.ko 2>/dev/null
insmod /root/snd-seq-dummy.ko 2>/dev/null
insmod /root/snd-ump.ko 2>/dev/null
insmod /root/mc.ko 2>/dev/null
insmod /root/snd-usbmidi-lib.ko 2>/dev/null
insmod /root/snd-usb-audio.ko 2>/dev/null

printf %s "$PRODUCT_CODE" > /root/fake-dt/inmusic,product-code
# Real hardware (both Prime 4 and Prime Go) has this rotation value set to
# compensate for a physically rotated panel. Our virtual/VNC output has no such
# physical rotation to correct for, so feeding it a nonzero value just rotates
# an already-correct image. Always force 0 here regardless of a prior run's state.
printf '\x00\x00\x00\x00' > /root/fake-dt/rotation

echo "=== Stopping any previous bridge/Engine instances ==="
for p in $(ps aux | grep '[v]nctouchbridge' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
for p in $(ps aux | grep '[E]ngine -d0' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
sleep 1

echo "=== Starting VNC touch bridge ==="
/root/vnctouchbridge $BRIDGE_PORT 127.0.0.1 $UPSTREAM_PORT 1280 720 > /root/bridge.log 2>&1 &
sleep 1

TOUCH_DEV=$(grep -A8 VncTouchBridge /proc/bus/input/devices | grep Handlers | sed -n 's/.*event\([0-9]*\).*/\1/p')
if [ -z "$TOUCH_DEV" ]; then
    echo "ERROR: could not find VncTouchBridge event device — check /root/bridge.log"
    exit 1
fi
echo "Touch device: /dev/input/event${TOUCH_DEV}"

echo "=== Launching Engine as ${PRODUCT_CODE} (Prime 4) ==="

QT_QPA_PLATFORM=vnc:size=${VNC_SIZE} \
QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/event${TOUCH_DEV} \
LD_PRELOAD=/root/dtshim.so:/root/crashhandler.so \
LD_LIBRARY_PATH=/usr/lib/glvnd-shim:/usr/qt/lib \
/usr/Engine/Engine -d0 > /root/engine.log 2>&1 &

sleep 2
echo "=== Done ==="
echo "Connect a VNC client to port ${BRIDGE_PORT} on this guest."
echo "Engine log: /root/engine.log   Bridge log: /root/bridge.log"