#!/bin/bash
# trial.sh [-H HOST] NAME SECONDS [VAR=value ...] : load the first track of the
# attached stick on both decks by touch, play both, and check the capture.
#
# The instance must be booted with boot.sh (or deploy.sh) and have sshd running
# and a tone image attached. With -H the guest runs on that ssh destination and
# its capture is checked there, while the taps go from here over VNC. Variables,
# if given, go through setenv.sh first and the script waits out Engine's restart.
#
# The coordinates are for Modern Engine's 1280x800 layout with the library
# showing. Load is by touch because midisurface's load command does nothing on
# some 5.x builds (JC11 5.3.0); play goes through midisurface.
set -eu
source "$(dirname "$0")/common.sh"
host=
if [ "${1:-}" = -H ]; then host=$2; shift 2; fi
name=$1; secs=$2; shift 2
port=$(instance_var "$name" SSH_PORT); display=$(instance_var "$name" VNC_DISPLAY)
remote="${QENGINE_REMOTE:-qengine}"
if [ -n "$host" ]; then
    on() { ssh "$host" "cd $remote && $*"; }
    srv="${host#*@}::$((5900 + display))"
else
    on() { (cd "$ROOT" && bash -c "$*"); }
    srv="localhost::$((5900 + display))"
fi

tap() { vnc "$srv" move "$1" "$2" mousedown 1 pause 0.3 mouseup 1 pause 3; }
double_tap() {
    vnc "$srv" move "$1" "$2" mousedown 1 pause 0.12 mouseup 1 pause 0.15 \
        mousedown 1 pause 0.12 mouseup 1 pause 6
}

if [ $# -gt 0 ]; then
    on "scripts/test/setenv.sh $port $*" > /dev/null
    # Engine ignores touch for about 90 to 110 s after a restart.
    sleep 110
fi
vnc "$srv" capture "$OUT/trial-$name-before.png"
double_tap 800 106; tap 486 525; sleep 4      # first track, then deck 1
tap 40 40; double_tap 800 106; tap 794 525    # back to the list, then deck 2
sleep 12
on "source scripts/test/common.sh && guest $port 'echo \"play left\" > /run/midisurface.fifo; sleep 2; echo \"play right\" > /run/midisurface.fifo'"
sleep "$secs"
vnc "$srv" capture "$OUT/trial-$name.png"
on "source scripts/test/common.sh && python3 scripts/test/wavcheck.py \$OUT/$name.wav --tone --last $((secs - 8)) &&
    guest $port 'grep -E \"period_size|buffer_size\" /proc/asound/card0/pcm0p/sub0/hw_params | tr \"\n\" \" \"; grep -E \"state|avail_max\" /proc/asound/card0/pcm0p/sub0/status | tr \"\n\" \" \"; echo; systemctl show engine -p NRestarts'"
