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

    # /dev/mem is remapped here so a probe of physical memory reads zeros instead of
    # real physical memory, which is what the hardware anti-clone check looks at.
    # Both architectures need it: dtshim remaps /dev/mem on either, and until now
    # only the armv7 builder created the file, so an arm64 guest got ENOENT instead.
    #
    # Sparse, and large enough to cover the addresses a guest actually maps. It used
    # to be zero-length, on the assumption that an mmap of an empty file would fail
    # cleanly -- it does not. The mmap succeeds and the first touch raises SIGBUS,
    # because every page is past the end of the file. That crash-looped MPC (SIGBUS
    # every ~3s, invisible until NRestarts was checked) once it resolved its product
    # code and got far enough to probe: it maps RK3288 register space near
    # 0xFF000000, so the file has to reach past 0xFF800000 (4283MiB) for those pages
    # to exist. 4400MiB does, and being sparse it costs ~0 on disk -- 32MiB actually
    # allocated in a built image.
    truncate -s 4400M "$_rootfs/root/fake-dev-mem"
}
