# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Writes the identity half of the fake devicetree: the two properties that are worth
# setting per instance.
#
#   write_fake_dt <rootfs mount point> <product code> [serial] [soc]
#
# Everything else dtshim serves it serves from itself, compiled in per SoC -- the
# rotation cells, RK3288's pcb-rev and internal-sd-fitted, and the /proc/interrupts
# fallback. See DT_REMAPS in shims/dtshim/dtshim.c. The split is identity here,
# fixed hardware description there. `soc` is "rk3288" or "rk3588" (default none);
# it gates the RK3288 secure-eFuse fixtures below.
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
    _soc="${4:-}"
    [ -n "$_code" ] || { echo "ERROR: write_fake_dt needs a product code" >&2; return 1; }

    mkdir -p "$_rootfs/root/fake-dt"
    printf '%s' "$_code"   > "$_rootfs/root/fake-dt/inmusic,product-code"
    printf '%s' "$_serial" > "$_rootfs/root/fake-dt/serial-number"

    # RK3288 Engine performs board validation against two secure eFuse nvmem
    # devices before constructing QGuiApplication. RK3588 (RMZ2) has no such gate,
    # so these fixtures are written only for rk3288 builds:
    #   ffb10000.efuse  "rockchip,rk3288-secure-efuse" -- word-addressed product
    #                   code, one char per 32-bit word, read back by the vendor
    #                   kernel as a byte at each word boundary.
    #   ffb40000.efuse  "rockchip,rk3288-efuse" -- byte-addressed CPU id nvmem
    #                   cell at offset 7 (16 bytes) plus cpu_leakage at 23.
    # QEMU's generic virt machine models neither, so write stand-in files and
    # let dtshim advertise + redirect them (remap_secure_efuse / readdir64).
    if [ "$_soc" = "rk3288" ]; then
        mkdir -p "$_rootfs/root/fake-dt/ffb10000.efuse/rockchip-efuse"
        _secure_efuse="$_rootfs/root/fake-dt/ffb10000.efuse/rockchip-efuse/nvmem"
        : > "$_secure_efuse"
        truncate -s 128 "$_secure_efuse"
        printf '%s' "$_code" | dd of="$_secure_efuse" conv=notrunc status=none

        # The CPU-id cell holds "RK3288" + board + product. The board is product-
        # specific: az05 for the JP11S/JP20/JP21 generation, az01 for everything
        # else (JC11S, JC11, JC16, JP11, JP13, JP14, JP07, JP08, NH08). Derived
        # here rather than hardcoded so a JP11S/JP20/JP21 image gets the right
        # platform in its CPU id too.
        case "$_code" in
            JP11S|JP20|JP21) _board="AZ05" ;;
            *)               _board="AZ01" ;;
        esac
        mkdir -p "$_rootfs/root/fake-dt/ffb40000.efuse/rockchip-efuse"
        _cpuid_efuse="$_rootfs/root/fake-dt/ffb40000.efuse/rockchip-efuse/nvmem"
        : > "$_cpuid_efuse"
        truncate -s 128 "$_cpuid_efuse"
        printf '%s' "RK3288${_board}${_code}" |
            dd of="$_cpuid_efuse" bs=1 seek=7 conv=notrunc status=none
    fi

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
