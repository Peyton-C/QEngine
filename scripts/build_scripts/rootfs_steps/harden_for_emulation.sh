# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# The two edits every emulated guest wants, whatever device it is pretending to
# be: no telemetry, and a root login that works over the serial console.
#
#   harden_for_emulation [rootfs mount point]    (default /mnt/rootfs)
#
# Neither knows anything about the architecture or the device, which is what
# makes them shareable. See docs/BLOCKING_TELEMETRY.md for the Sentry host.
harden_for_emulation() {
    _rootfs="${1:-/mnt/rootfs}"

    echo "--- blocking Sentry telemetry (docs/BLOCKING_TELEMETRY.md) ---"
    _telemetry_line="127.0.0.1 o230257.ingest.sentry.io"
    grep -qxF "$_telemetry_line" "$_rootfs/etc/hosts" \
        || echo "$_telemetry_line" >> "$_rootfs/etc/hosts"

    echo "--- blanking root password for serial-console login ---"
    sed -i 's|^root:[^:]*:|root::|' "$_rootfs/etc/shadow"
}
