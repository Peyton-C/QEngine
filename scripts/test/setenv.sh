#!/bin/bash
# setenv.sh PORT [VAR=value ...] : restart Engine with extra environment, such
# as ALSASHIM_BUFFER_SCALE=4. With no variables it clears what an earlier call set.
#
# The drop-in goes under /run/systemd/system, so a reboot undoes it and the
# rootfs keeps what the builder wrote.
set -eu
source "$(dirname "$0")/common.sh"
port=$1; shift
conf=/run/systemd/system/engine.service.d/zz-test.conf
lines="[Service]"
for v in "$@"; do lines="$lines\nEnvironment=$v"; done
guest "$port" "mkdir -p ${conf%/*}; printf '$lines\n' > $conf; cat $conf; systemctl daemon-reload; systemctl restart engine"
