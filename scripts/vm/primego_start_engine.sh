#!/bin/sh
# run-primego.sh — launch Engine spoofed as Prime Go (JP11), with VNC output
# and real mouse-to-touch input via vnctouchbridge.
#
# Run this from inside the guest, after boot, as root. Safe to re-run without
# rebooting — kills any previous bridge/Engine instances first.

PRODUCT_CODE=JP11
# Prime Go's real panel is native portrait 800x1280, physically mounted
# rotated (devicetree rotation=0x10e/270 on real hardware — see fake-dt/rotation).
VNC_SIZE=800x1280
SCREEN_W=800
SCREEN_H=1280
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
# Real hardware has this set to compensate for a physically rotated panel. Our
# virtual/VNC output has no such physical rotation to correct for (and setting
# it didn't fix Prime Go's rendering issue anyway), so always force 0 here.
printf '\x00\x00\x00\x00' > /root/fake-dt/rotation

echo "=== Stopping any previous bridge/Engine instances ==="
for p in $(ps aux | grep '[v]nctouchbridge' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
for p in $(ps aux | grep '[E]ngine -d0' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
sleep 1

echo "=== Starting VNC touch bridge ==="
/root/vnctouchbridge $BRIDGE_PORT 127.0.0.1 $UPSTREAM_PORT $SCREEN_W $SCREEN_H > /root/bridge.log 2>&1 &
sleep 1

TOUCH_DEV=$(grep -A8 VncTouchBridge /proc/bus/input/devices | grep Handlers | sed -n 's/.*event\([0-9]*\).*/\1/p')
if [ -z "$TOUCH_DEV" ]; then
    echo "ERROR: could not find VncTouchBridge event device — check /root/bridge.log"
    exit 1
fi
echo "Touch device: /dev/input/event${TOUCH_DEV}"

echo "=== Launching Engine as ${PRODUCT_CODE} (Prime Go) ==="
echo "NOTE: Prime Go has a battery. Engine will show 'Touch and hold the"
echo "Engine DJ logo to turn on' and auto-quit after ~30s if not touched."
echo "Connect a VNC client to port ${BRIDGE_PORT} quickly and touch-and-hold"
echo "the logo to get past this gate."

QT_QPA_PLATFORM=vnc:size=${VNC_SIZE} \
QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/event${TOUCH_DEV} \
LD_PRELOAD=/root/dtshim.so:/root/crashhandler.so \
LD_LIBRARY_PATH=/usr/lib/glvnd-shim:/usr/qt/lib \
/usr/Engine/Engine -d0 > /root/engine.log 2>&1 &

sleep 2
echo "=== Done ==="
echo "Connect a VNC client to port ${BRIDGE_PORT} on this guest."
echo "Engine log: /root/engine.log   Bridge log: /root/bridge.log"