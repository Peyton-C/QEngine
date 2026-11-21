#!/bin/bash
# tap.sh HOST::PORT OUT.png [X Y]... : tap each point on the guest's VNC
# display, then save a screenshot.
#
# Each tap is held for 0.3 s. Engine ignores a press and release that arrive
# together, as vncdo's own click sends them.
set -eu
source "$(dirname "$0")/common.sh"
srv=$1; out=$2; shift 2
args=()
while [ $# -ge 2 ]; do
    args+=(move "$1" "$2" mousedown 1 pause 0.3 mouseup 1 pause 1.5)
    shift 2
done
vnc "$srv" ${args[@]+"${args[@]}"} pause 1 capture "$out"
