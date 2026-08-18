# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Writes the devicetree properties every Engine guest needs, whatever device it is
# pretending to be. dtshim serves these to Engine in place of the real devicetree.
#
#   write_fake_dt <rootfs mount point> <product code>
#
# Three properties are common to every product: the code that decides which
# device this is, a serial number, and a zero rotation. Each builder writes its
# own SoC's extra properties after calling this -- dtshim remaps a fixed list per
# SoC and a missing target turns a working read into ENOENT, so the extras cannot
# be guessed here.
#
# Written rather than copied from committed fixtures so that changing which device
# an image spoofs needs no edit to a tracked file, and so both builders spell it
# the same way.
write_fake_dt() {
    _rootfs="$1"
    _code="$2"
    [ -n "$_code" ] || { echo "ERROR: write_fake_dt needs a product code" >&2; return 1; }

    mkdir -p "$_rootfs/root/fake-dt"
    printf '%s' "$_code" > "$_rootfs/root/fake-dt/inmusic,product-code"
    printf '%s' 'QENGINE0001SIM' > "$_rootfs/root/fake-dt/serial-number"
    # Raw big-endian <u32> devicetree cell, not text.
    printf '\x00\x00\x00\x00' > "$_rootfs/root/fake-dt/rotation"
}
