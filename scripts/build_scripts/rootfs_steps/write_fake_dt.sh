# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Writes the identity half of the fake devicetree: the two properties that are worth
# setting per instance.
#
#   write_fake_dt <rootfs mount point> <product code> [serial]
#
# Everything else dtshim serves it serves from itself, compiled in per SoC -- the
# rotation cells, RK3288's pcb-rev and internal-sd-fitted, and the /proc/interrupts
# fallback. See DT_REMAPS in shims/dtshim/dtshim.c. The split is identity here,
# fixed hardware description there.
#
# These two stay files because they vary and because something other than dtshim
# reads them: Engine shows the serial in Settings as DeviceSerialNumber, and
# midisurface opens the product-code file directly to decide which device to answer
# Engine's inquiry as. Writing them rather than shipping fixtures also means
# changing which device an image spoofs needs no edit to a tracked file.
write_fake_dt() {
    _rootfs="$1"
    _code="$2"
    _serial="${3:-QENGINE0001SIM}"
    [ -n "$_code" ] || { echo "ERROR: write_fake_dt needs a product code" >&2; return 1; }

    mkdir -p "$_rootfs/root/fake-dt"
    printf '%s' "$_code"   > "$_rootfs/root/fake-dt/inmusic,product-code"
    printf '%s' "$_serial" > "$_rootfs/root/fake-dt/serial-number"

    # /dev/mem is remapped here so a mmap of it fails cleanly rather than handing out
    # real physical memory, which is what the hardware anti-clone check probes. Left
    # as a file rather than served from the shim so that failure mode does not move.
    # Both architectures need it: dtshim remaps /dev/mem on either, and until now
    # only the armv7 builder created the file, so an arm64 guest got ENOENT instead.
    : > "$_rootfs/root/fake-dev-mem"
}
