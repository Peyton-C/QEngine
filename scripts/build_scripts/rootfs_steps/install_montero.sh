# install_montero — put MONTERO and the mods build_montero.sh staged into an
# Engine rootfs, and preload it into Engine.
#
#   install_montero <rootfs> <shim arch> <stage dir>
#
# Run after engine.service.d/override.conf is written: it appends to that
# file's LD_PRELOAD line. Last in the list, since MONTERO only adds resources
# and QML and has no reason to be ahead of the shims that make Engine start at
# all. Its log is /tmp/montero.log in the guest.
#
# The mods go in /root/montero/mods, the first place MONTERO looks, rather than
# /data/mods: /data is the data disk's mount point, so anything written under it
# here would be hidden at boot. /data/mods still works for mods added by hand.
install_montero() {
    _root="$1"
    _arch="$2"
    _stage="$3"
    _override="$_root/etc/systemd/system/engine.service.d/override.conf"

    echo "--- inserting MONTERO and its mods ---"
    grep -q '^Environment=LD_PRELOAD=' "$_override" || {
        echo "ERROR: no LD_PRELOAD line in $_override to add MONTERO to." >&2
        return 1
    }
    cp -a "/shims/montero/montero_$_arch.so" "$_root/root/montero.so"
    chmod 755 "$_root/root/montero.so"
    rm -rf "$_root/root/montero"
    mkdir -p "$_root/root/montero/mods"
    cp -a "$_stage/montero-mods/." "$_root/root/montero/mods/"
    sed -i 's|^Environment=LD_PRELOAD=.*|&:/root/montero.so|' "$_override"
}
