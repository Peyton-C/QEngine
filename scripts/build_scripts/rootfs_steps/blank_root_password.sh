# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Clears root's password hash so the serial console gives a shell without one.
# Emulated guests have no other way in: there is no SSH daemon on a stock Engine
# OS rootfs, and the password on real hardware is unknown.
#
#   blank_root_password [rootfs mount point]    (default /mnt/rootfs)
#
# This is deliberately the opposite of hardening, and is why these images are for
# emulation only. On a device whose display is fullscreen the getty should be
# taken away too, or stray keystrokes reach this shell — see build_mpc_rootfs.sh,
# which disables tty1 for exactly that reason.
blank_root_password() {
    _rootfs="${1:-/mnt/rootfs}"

    echo "--- blanking root password for serial-console login ---"
    sed -i 's|^root:[^:]*:|root::|' "$_rootfs/etc/shadow"
}
