# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Puts a virgl-capable Mesa into the mounted rootfs, in whichever of the two driver
# layouts that rootfs uses.
#
#   install_virgl_mesa <layout> <version> [rootfs mount point] [stage dir]
#                                          (default /mnt/rootfs)  (default /stage)
#
# Both builders needed this and each had its own copy, written against the layout
# its own guest happened to use -- which is exactly the assumption detect_mesa.sh
# exists to remove, since a guest's layout follows its firmware and not its
# architecture. One step, taking the layout as an argument, cannot drift that way.
#
# What each layout needs is genuinely different, not just differently named:
#
#   dri      Additive. /usr/lib/dri holds several dozen *_dri.so hardlinks to one
#            megadriver that has no virgl in it. virtio_gpu_dri.so is the name
#            Mesa's loader derives from the kernel's device name and nothing
#            already present uses it, so the file is simply dropped in and every
#            existing driver still works.
#   gallium  Destructive, and unavoidably so. Every driver is compiled inside
#            /usr/lib/libgallium-<ver>.so and libEGL links that file directly,
#            resolving against a symbol version node named after it. There is no
#            plug-in slot -- no /usr/lib/dri, no gallium-pipe directory -- so
#            adding virgl means replacing the library. The vendor file is kept
#            alongside as .vendor, because the replacement drops the hardware
#            drivers and so is right for emulation and wrong for a real unit.
#   none     Nothing to do, and nothing that could be done: armv7 firmware before
#            5.0.0 has no Mesa at all and gets GL from a proprietary Mali blob
#            providing libEGL itself. A driver installed here would never be
#            opened, so this says so instead of leaving a file that looks like a
#            fix.
#
# Without virgl in whichever library the guest actually loads, GL is rasterized on
# an emulated CPU, and the difference in frame time is two orders of magnitude.
install_virgl_mesa() {
    _layout="$1"
    _version="$2"
    _rootfs="${3:-/mnt/rootfs}"
    _stage="${4:-/stage}"

    case "$_layout" in
    none)
        echo "--- no Mesa in this rootfs, so no virgl to install ---"
        return 0
        ;;
    dri)
        echo "--- installing a virgl-capable DRI driver alongside the vendor Mesa ---"
        [ -s "$_stage/virtio_gpu_dri.so" ] || {
            echo "ERROR: no virtio_gpu_dri.so staged for this rootfs." >&2; return 1; }
        mkdir -p "$_rootfs/usr/lib/dri"
        cp -a "$_stage/virtio_gpu_dri.so" "$_rootfs/usr/lib/dri/virtio_gpu_dri.so"
        chown 0:0 "$_rootfs/usr/lib/dri/virtio_gpu_dri.so"
        ;;
    gallium)
        echo "--- installing a virgl-capable libgallium over the vendor one ---"
        _so="libgallium-$_version.so"
        [ -s "$_stage/$_so" ] || {
            echo "ERROR: no $_so staged for this rootfs." >&2; return 1; }
        # The version has to match what libEGL resolves against, so a rootfs that
        # does not have this exact file is a detection or staging mismatch rather
        # than something to install over.
        [ -e "$_rootfs/usr/lib/$_so" ] || {
            echo "ERROR: this rootfs has no /usr/lib/$_so to replace." >&2
            echo "       detect_mesa.sh and the staged library disagree." >&2
            return 1; }
        # Idempotent: these builders get re-run against an already-built rootfs,
        # and the backup must stay the vendor file rather than becoming a copy of
        # the previous run's replacement.
        [ -e "$_rootfs/usr/lib/$_so.vendor" ] ||
            cp -a "$_rootfs/usr/lib/$_so" "$_rootfs/usr/lib/$_so.vendor"
        cp -a "$_stage/$_so" "$_rootfs/usr/lib/$_so"
        # cp -a carried the building user's uid in from the host; every other file
        # in /usr/lib is root's.
        chown 0:0 "$_rootfs/usr/lib/$_so"
        ;;
    *)
        echo "ERROR: unknown Mesa layout '$_layout'." >&2
        return 1
        ;;
    esac
}
