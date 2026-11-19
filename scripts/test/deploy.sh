#!/bin/bash
# deploy.sh HOST NAME : copy an instance to a test host and boot it there, then
# start sshd in it and attach the test tones.
#
# HOST is an ssh destination such as user@qengine-1.local. Build on the Mac and
# run on the host: the builders need Docker and a fast disk, the host only QEMU.
# The host keeps a copy of scripts/ and the shared kernels under
# QENGINE_REMOTE (default ~/qengine), and this refreshes both. Stop any guest
# already running there first, since every one uses the same serial and
# monitor ports.
set -eu
source "$(dirname "$0")/common.sh"
host=$1; name=$2; remote="${QENGINE_REMOTE:-qengine}"
kernel=$(instance_var "$name" KERNEL_IMG); initrd=$(instance_var "$name" INITRD_IMG)
port=$(instance_var "$name" SSH_PORT); display=$(instance_var "$name" VNC_DISPLAY)

ssh "$host" "mkdir -p $remote/build/instances $remote/build/test"
rsync -a "$ROOT/scripts/" "$host:$remote/scripts/"
rsync -a "$kernel" "$kernel.info" "$initrd" "$host:$remote/build/"
rsync -a --sparse -z "$ROOT/build/instances/$name" "$host:$remote/build/instances/"
rsync -a "$KEY" "$KEY.pub" "$host:$remote/build/test/"
[ ! -f "$OUT/usb.img" ] || rsync -a --sparse "$OUT/usb.img" "$host:$remote/build/test/"

# instance.env holds absolute paths into this checkout; point them at the host's.
ssh "$host" "cd $remote && sed -i \"s#$ROOT#\$PWD#\" build/instances/$name/instance.env &&
    scripts/test/boot.sh $name > /dev/null && sleep 75 && scripts/test/sshd.sh &&
    scripts/test/attach.sh $port"
echo "guest ssh port $port, vnc ${host#*@}::$((5900 + display))"
