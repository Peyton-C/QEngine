#!/bin/bash
# boot.sh NAME [LABEL] : boot an instance headless in the background, capturing
# its audio to $OUT/LABEL.wav instead of playing it.
#
# Serial console on $SERIAL_PORT, QEMU monitor on $MONITOR_PORT, software
# rendering, so it needs no GPU or display and runs the same on a Pi over ssh.
# TCP rather than unix sockets for both: an instance path under build/ makes a
# socket path longer than the 104 bytes macOS allows. The capture uses the wav
# audiodev, which keeps the guest's timing, so a gap in the file is a gap Engine
# would have played. Extra QEMU arguments in QEMU_EXTRA_ARGS are kept.
set -eu
source "$(dirname "$0")/common.sh"
name=$1; wav="$OUT/${2:-$1}.wav"; log="$OUT/qemu-$name.log"
cd "$ROOT"
SERIAL="tcp:127.0.0.1:$SERIAL_PORT,server,nowait" \
QEMU_EXTRA_ARGS="-monitor tcp:127.0.0.1:$MONITOR_PORT,server,nowait ${QEMU_EXTRA_ARGS:-}" \
AUDIODEV_BACKEND="wav,path=$wav" \
    nohup scripts/qemu/run_instance.sh --name "$name" --display vnc --no-gl \
    > "$log" 2>&1 < /dev/null &
sleep 5
cat "$log"
echo "capture: $wav"
