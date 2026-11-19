# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Keeps Engine OS's disk service, edisksd, from offering the guest's own virtio
# disks to the app as media drives.
#
#   hide_internal_disks [rootfs mount point]    (default /mnt/rootfs)
#
# edisksd mounts nothing on its own. It publishes every block device udev knows,
# with the udev property EDISKSD_IGNORE as the HintIgnore D-Bus property, and the
# app skips a device carrying it. Engine OS sets it on the device's eMMC in
# /usr/lib/udev/rules.d/90-edisks-<platform>.rules, matched by the SoC's MMC
# controller path (DEVPATH=="*/platform/ff0f0000.dwmmc/*" on RK3288). Under QEMU
# the rootfs and /data are virtio disks, that rule never matches, and Engine puts
# up "Incompatible Format" for each ext4 partition at startup (JP24 5.3.0 shows one
# per partition of rootfs.img and data_disk.img).
#
# Every virtio disk is the device's internal storage, so all of them are marked,
# the way scripts/pi/chroot-up.sh marks the Pi's own SD card. USB sticks attached
# by scripts/test/attach.sh are loop devices and stay visible. The drum module's
# internal SD card is the exception: on the device it sits on ff0c0000.dwmmc,
# which Engine OS does not ignore, so it stays a drive here too, found by the UUID
# make_disk.sh formats it with.
#
# Hiding the disks from udev altogether, by clearing their ID_FS_* properties, also
# stops the dialog, but the guest's own mount units find /data by those properties.
hide_internal_disks() {
    _rootfs="${1:-/mnt/rootfs}"

    echo "--- hiding the guest's virtio disks from edisksd ---"
    mkdir -p "$_rootfs/etc/udev/rules.d"
    # Written whole each time, so a second run leaves the same file.
    cat > "$_rootfs/etc/udev/rules.d/90-qengine-edisks.rules" <<'EOF'
# Written by QEngine: the virtio disks are this device's internal storage.
SUBSYSTEM=="block", KERNEL=="vd*", ENV{ID_FS_UUID}!="5d2a1e5c-0a1b-4c2d-9e3f-51d0c0a1e5d0", ENV{EDISKSD_IGNORE}="1"
EOF
}
