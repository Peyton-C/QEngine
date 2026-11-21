#!/bin/bash
# attach.sh PORT [IMAGE] : copy a FAT image into the guest and attach it as a
# loop device, which edisksd mounts under /media like a USB stick. Then print
# what the guest's audio looks like.
#
# Losetup rather than QEMU's USB storage: it needs no host-side hotplug, works
# on every kernel the instances use, and goes away with the guest.
set -eu
source "$(dirname "$0")/common.sh"
port=$1; img="${2:-$OUT/usb.img}"
scp -q -P "$port" -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null "$img" root@localhost:/data/test.img
guest "$port" '
losetup -P -f /data/test.img; sleep 5
echo "media: $(ls /media | tr "\n" " ")"
grep -E "^ ?[0-9]" /proc/asound/cards
grep -hE "^Environment=\"?ALSASHIM" /etc/systemd/system/engine.service.d/*.conf 2>/dev/null
echo "engine: $(systemctl is-active engine), $(systemctl show engine -p NRestarts)"'
