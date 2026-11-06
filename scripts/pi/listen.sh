#!/bin/bash
# listen.sh — plays a Pi's Engine audio on this machine, over SSH.
#
# Usage: listen.sh <user@pi> [first-channel]
#   Run it on the machine you want to hear Engine on, with Engine started on the
#   Pi as `AUDIO_CARD=Loopback` (see README.md). Needs sox here (`play`) and
#   nothing extra on the Pi. Ctrl-C stops it; Engine is not affected.
#
#   first-channel picks the stereo pair to listen to, counting from 1. Engine
#   lays its outputs out in pairs; on a Prime 4 G2 the master is 1 (the default),
#   and 5, 7 and 9 carry other outputs.
#
# Expect a delay of a few hundred milliseconds.
set -euo pipefail

HOST="${1:-}"
FIRST="${2:-1}"
[ -n "$HOST" ] || { echo "usage: $0 <user@pi> [first-channel]" >&2; exit 1; }
command -v play >/dev/null 2>&1 || { echo "ERROR: needs sox (play)." >&2; exit 1; }

# Engine opens the loopback card with every channel it offers (32, as 32-bit
# samples), so that is what the capture end has to ask for; a narrower request
# goes through ALSA's conversion layer, which mixes channels down rather than
# picking two. The pair is cut out on the Pi so only stereo crosses the network.
ssh -o ServerAliveInterval=15 "$HOST" "arecord -q -D hw:Loopback,1,0 -f S32_LE -r 44100 -c 32 -t raw | python3 -c '
import array, sys
first = $FIRST - 1
src, dst = sys.stdin.buffer, sys.stdout.buffer
frame = 32 * 4
while True:
    data = src.read(frame * 256)
    if not data:
        break
    a = array.array(\"i\")
    a.frombytes(data[:len(data) // frame * frame])
    out = array.array(\"i\", bytes(len(a) // 32 * 8))
    out[0::2] = a[first::32]
    out[1::2] = a[first + 1::32]
    dst.write(out.tobytes())
    dst.flush()
'" | play -q -t raw -r 44100 -e signed -b 32 -c 2 -
