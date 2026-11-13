#!/bin/bash
# chroot-up.sh — prepares an Engine OS rootfs to be run in a chroot on a
# Raspberry Pi 5 (or any arm64 Linux host), instead of as a QEMU guest.
#
# Usage: sudo chroot-up.sh
#   Idempotent; run it once per boot, then chroot-engine.sh. See README.md for
#   what has to be in the rootfs first.
#
# Environment (or $QENGINE_DIR/pi.env, which both scripts read):
#   QENGINE_DIR  where rootfs.img lives and data.img is created.
#                Default: ~/qengine of the user who ran sudo.
#   ENGINE_ROOT  where the rootfs is mounted. Default: /srv/engine
#   IGNORE_DRIVES  USB drives Engine is not to mount, by serial number or
#                volume label, separated by spaces. Default: none.
set -euo pipefail

QENGINE_DIR="${QENGINE_DIR:-$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/qengine}"
# Settings file first, then the caller's environment again on top of it, so that
# `sudo AUDIO_CARD=... chroot-engine.sh` overrides the file for one run.
_caller_env="$(export -p)"
[ -f "$QENGINE_DIR/pi.env" ] && . "$QENGINE_DIR/pi.env"
eval "$_caller_env"
R="${ENGINE_ROOT:-/srv/engine}"

[ -f "$QENGINE_DIR/rootfs.img" ] || {
    echo "ERROR: no rootfs.img in $QENGINE_DIR (set QENGINE_DIR)." >&2; exit 1; }

mkdir -p "$R"
# The mount point is bound onto itself and made private before anything goes
# on it. Services that run in a mount namespace of their own (logind, udevd,
# resolved, colord, ...) otherwise get a copy of every mount made here, which
# no unmount on the host reaches once /sys and /dev are slaves, and which keeps
# the loop devices busy until the next boot. Only the empty bind propagates.
if ! mountpoint -q "$R"; then
    mount --bind "$R" "$R" && mount --make-private "$R"
fi
findmnt -n -o SOURCE --mountpoint "$R" | grep -q '^/dev/loop' ||
    mount -o loop "$QENGINE_DIR/rootfs.img" "$R"

# /data must be ext4 with the encrypt feature: Engine fscrypts two directories
# under it at every launch (usr/Engine/Scripts/encrypt-fs.sh), and a host root
# filesystem will not normally have that feature turned on.
if [ ! -e "$QENGINE_DIR/data.img" ]; then
    truncate -s "${DATA_SIZE:-2G}" "$QENGINE_DIR/data.img"
    mkfs.ext4 -q -O encrypt -L data "$QENGINE_DIR/data.img"
fi
mountpoint -q "$R/data" || mount -o loop "$QENGINE_DIR/data.img" "$R/data"

mountpoint -q "$R/proc" || mount -t proc proc "$R/proc"
mountpoint -q "$R/sys"  || mount --rbind /sys "$R/sys"
mountpoint -q "$R/dev"  || mount --rbind /dev "$R/dev"
mountpoint -q "$R/run"  || mount -t tmpfs -o mode=0755 tmpfs "$R/run"
# Qt finds input devices through libudev, which reads device properties from
# /run/udev. Without the host database Engine opens no touchscreen at all.
mkdir -p "$R/run/udev"
mountpoint -q "$R/run/udev" || mount --bind /run/udev "$R/run/udev"
# Where Engine OS's disk service (edisksd) mounts removable drives; a tmpfs on
# the real system too (media.mount).
mountpoint -q "$R/media" || mount -t tmpfs -o mode=1777 tmpfs "$R/media"
mountpoint -q "$R/tmp"   || mount -t tmpfs tmpfs "$R/tmp"
# Yocto's, which Engine OS 4.4 and later has. Buildroot's (4.3.4 and earlier)
# has no /var/volatile.
if [ -d "$R/var/volatile" ]; then
    mountpoint -q "$R/var/volatile" || mount -t tmpfs tmpfs "$R/var/volatile"
    mkdir -p "$R/var/volatile/log" "$R/var/volatile/tmp"
fi

# edisksd mounts every block device udev shows it unless told otherwise, and in
# a chroot it sees the host's. Engine OS marks its own internal storage with
# these properties (90-edisks-az04.rules); this does the same for this
# machine's, so only real removable drives are offered to Engine as media.
# IGNORE_DRIVES adds USB drives, by serial number or volume label, for something
# else on the machine to have, such as the XDJ-RX3 player.
RULE=/etc/udev/rules.d/90-qengine-edisks.rules
RULES='# Written by qengine chroot-up.sh: keep Engine OS'"'"'s disk service off this
# machine'"'"'s own storage, and off the drives IGNORE_DRIVES in pi.env names.
SUBSYSTEM=="block", KERNEL=="mmcblk*|loop*|zram*|ram*|nvme*", ENV{EDISKSD_IGNORE}="1"'
for d in ${IGNORE_DRIVES:-}; do
    case "$d" in *[!A-Za-z0-9._-]*)
        echo "WARNING: IGNORE_DRIVES: skipping '$d' (letters, digits, . _ - only)" >&2; continue ;;
    esac
    RULES+=$'\n'"SUBSYSTEM==\"block\", ENV{ID_SERIAL_SHORT}==\"$d\", ENV{EDISKSD_IGNORE}=\"1\""
    RULES+=$'\n'"SUBSYSTEM==\"block\", ENV{ID_FS_LABEL}==\"$d\", ENV{EDISKSD_IGNORE}=\"1\""
done
if [ "$(cat "$RULE" 2>/dev/null)" != "$RULES" ]; then
    printf '%s\n' "$RULES" > "$RULE"
    udevadm control --reload
    udevadm trigger --subsystem-match=block --action=change
    udevadm settle
fi

# Engine OS's own rule for which partitions are not media at all (EFI system
# partitions, swap, LVM members and the like). edisksd reads the result from
# udev, and here that is the host's udev, so the rule has to live on the host.
VENDOR_RULE="$R/usr/lib/udev/rules.d/80-edisksd.rules"
if [ -f "$VENDOR_RULE" ] && ! cmp -s "$VENDOR_RULE" /etc/udev/rules.d/80-edisksd.rules; then
    cp "$VENDOR_RULE" /etc/udev/rules.d/80-edisksd.rules
    udevadm control --reload
    udevadm trigger --subsystem-match=block --action=change
    udevadm settle
fi

# Host side: what the Engine guest kernel/initrd normally provides.
modprobe -a uinput snd-seq snd-seq-midi snd-rawmidi snd-usb-audio 2>/dev/null || true
# The tty1 login prompt shares the display Engine renders to.
systemctl stop getty@tty1.service || true
echo "chroot ready at $R"
