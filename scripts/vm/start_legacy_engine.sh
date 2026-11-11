#!/bin/sh
# Qt 5.9's VNC mouse events do not replace a touchscreen. Start the uinput
# bridge before Qt discovers input devices, then point evdevtouch at that node.
set -eu
/root/vnctouchbridge 5900 127.0.0.1 5901 1280 800 > /run/qengine-touchbridge.log 2>&1 &
bridge_pid=$!
engine_pid=
cleanup() {
    [ -z "$engine_pid" ] || kill "$engine_pid" 2>/dev/null || true
    kill "$bridge_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' TERM INT
touch_dev=
count=0
while [ "$count" -lt 50 ]; do
    touch_dev=$(awk '/^N: Name=.*VncTouchBridge/{found=1}
        found && /^H:/{for(i=1;i<=NF;i++) if($i ~ /^event/){print "/dev/input/"$i;exit}}
        /^$/{found=0}' /proc/bus/input/devices)
    [ -z "$touch_dev" ] || break
    kill -0 "$bridge_pid" 2>/dev/null || { cat /run/qengine-touchbridge.log >&2; exit 1; }
    sleep 0.1
    count=$((count+1))
done
[ -n "$touch_dev" ] || { echo "QEngine: no VNC touchscreen appeared" >&2; exit 1; }
echo "QEngine touch input: $touch_dev"
if [ -f /etc/qengine/qt55-offscreen ]; then
    export QT_QPA_PLATFORM=offscreen
    export QMLSCENE_DEVICE=customcontext
    export QSG_RENDER_LOOP=basic
else
    export QT_QPA_PLATFORM=vnc:size=1280x800:port=5901
    export QT_QUICK_BACKEND=software
fi
export QT_QPA_GENERIC_PLUGINS="evdevtouch:$touch_dev"
if [ -x /usr/JP07/JP07 ]; then
    /usr/JP07/JP07 -d0 -card hw:Loopback,0 &
else
    # One product binary per image, such as JP07, JC11 or NH08, named as the device.
    # SC LIVE 2 2.3.3 ships none and runs /usr/Engine/Engine itself.
    app=/usr/Engine/Engine
    for candidate in /usr/Engine/[A-Z][A-Z][0-9][0-9]; do
        if [ -x "$candidate" ]; then app="$candidate"; break; fi
    done
    "$app" -d0 &
fi
engine_pid=$!
wait "$engine_pid"
