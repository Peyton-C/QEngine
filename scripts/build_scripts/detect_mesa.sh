# Sourced by the build scripts in this directory — not executed on its own.
#
# Reports which Mesa a rootfs ships and in which of the two driver layouts, so the
# virgl build can be matched to the guest instead of to a table.
#
#   detect_mesa <rootfs image>
#
# On success MESA_VERSION and MESA_LAYOUT are set. MESA_LAYOUT is one of:
#
#   gallium  /usr/lib/libgallium-<ver>.so — Mesa 24.3 and later. Every driver is
#            compiled into that one library and libEGL links it directly,
#            resolving against a symbol version node named after the file, so the
#            version is part of the ABI and the filename is authoritative. There
#            is no /usr/lib/dri in this layout at all.
#   dri      /usr/lib/dri/*_dri.so — Mesa 24.1 and earlier. The several dozen
#            entries are hardlinks to a single megadriver, so reading any one of
#            them answers for all.
#   none     no Mesa. Engine OS before 5.0.0 on armv7 gets GL from a proprietary
#            Mali blob that provides libEGL/libGLESv2/libGLESv1_CM itself; there
#            is no /usr/lib/dri, no libgbm, and nothing for a virgl driver to plug
#            into. MESA_VERSION is empty.
#
# Why detect rather than tabulate: both the version and the layout are set by the
# firmware, not by the architecture, and neither follows the other. The same
# firmware version ships different Mesa per SoC -- 5.0.0-5.0.4 is 24.0.7 on armv7
# and 24.3.4 on arm64 -- while a single architecture changes layout across its own
# versions, since RMZ2 4.5.0/4.6.0 are 24.1.0 in the DRI layout and RMZ2 5.0.x is
# 24.3.4 in the gallium one. A table keyed to the architecture gets one of those
# two facts wrong for every firmware it was not written against, and the failure is
# silent: a version mismatch in the gallium layout breaks GL outright, and in the
# DRI layout it is an ABI gamble against a loader from another release.
#
# The version is read from the megadriver, never from libEGL, libGLESv2 or libgbm.
# Those are loader stubs: the armv7 libEGL happens to contain a bare version string
# and the arm64 one contains none at all, so reading them looks like it works right
# up until the guest where it does not.
detect_mesa() {
    _img="$1"
    MESA_VERSION=""
    MESA_LAYOUT=""

    # Same host-tool lookup as run_qemu.sh's dumpe2fs: e2fsprogs installs into
    # /sbin on Debian, and Homebrew keeps it off PATH entirely.
    _dbg=""
    for _c in debugfs /sbin/debugfs /usr/sbin/debugfs \
              /opt/homebrew/opt/e2fsprogs/sbin/debugfs; do
        if command -v "$_c" >/dev/null 2>&1; then _dbg="$_c"; break; fi
    done
    [ -n "$_dbg" ] || {
        echo "ERROR: no debugfs found; install e2fsprogs." >&2; return 1; }

    # Read the image rather than mount it: this runs before the privileged
    # container, and detecting the version is what decides what that container is
    # given to install.
    _ls_usrlib="$("$_dbg" -R 'ls -p /usr/lib' "$_img" 2>/dev/null |
        tr '/' ' ' | awk '{print $5}')"

    # An unreadable image lists nothing, which is indistinguishable from a rootfs
    # that ships no Mesa unless it is checked for -- and answering "none" for an
    # image debugfs could not open would send a builder on to install nothing at
    # all and report success. Every rootfs these builders touch has a /usr/lib.
    [ -n "$_ls_usrlib" ] || {
        echo "ERROR: nothing readable at /usr/lib in $_img." >&2
        echo "       $("$_dbg" -R 'ls /' "$_img" 2>&1 | tail -n 1)" >&2
        echo "       Expected a raw ext2/3/4 image, as extract_rootfs produces." >&2
        return 1; }

    # The .vendor suffix is one of ours, kept by install_virgl_mesa when it
    # replaces the library, so an already-built image reports the same answer a
    # freshly extracted one does.
    # The selection is done by the last stage on purpose. A `grep | head -n 1`
    # would let head exit at the first line, kill grep with SIGPIPE, and -- under
    # the `set -o pipefail` these builders run with -- fail the assignment and
    # abort the build with nothing printed. awk reads its input to the end.
    _gallium="$(printf '%s\n' "$_ls_usrlib" |
        awk '/^libgallium-[0-9][0-9.]*\.so$/ && !seen++')"
    if [ -n "$_gallium" ]; then
        MESA_LAYOUT=gallium
        MESA_VERSION="${_gallium#libgallium-}"
        MESA_VERSION="${MESA_VERSION%.so}"
        echo "--- guest Mesa: $MESA_VERSION (gallium layout) ---"
        return 0
    fi

    # Any hardlink of the megadriver will do, except the one these builders add.
    _mega="$("$_dbg" -R 'ls -p /usr/lib/dri' "$_img" 2>/dev/null |
        awk -F/ '{ n = $6 }
                 n ~ /_dri\.so$/ && n != "virtio_gpu_dri.so" && !seen++ { print n }')"
    if [ -n "$_mega" ]; then
        _tmp="$(mktemp -d /tmp/qengine-mesa.XXXXXX)"
        "$_dbg" -R "dump /usr/lib/dri/$_mega $_tmp/mega.so" "$_img" 2>/dev/null
        # grep -a: the driver is a binary, and without it GNU grep reports a match
        # instead of printing it while BusyBox prints nothing at all.
        MESA_VERSION="$(grep -aoE 'Mesa [0-9]+\.[0-9]+(\.[0-9]+)?' "$_tmp/mega.so" |
            head -n 1 | sed 's/^Mesa //')"
        rm -rf "$_tmp"
        [ -n "$MESA_VERSION" ] || {
            echo "ERROR: /usr/lib/dri/$_mega carries no Mesa version string." >&2
            return 1; }
        MESA_LAYOUT=dri
        echo "--- guest Mesa: $MESA_VERSION (dri layout, read from $_mega) ---"
        return 0
    fi

    MESA_LAYOUT=none
    _blob="$(printf '%s\n' "$_ls_usrlib" | awk '/^libmali/ && !seen++')"
    if [ -n "$_blob" ]; then
        echo "--- guest has no Mesa: GL comes from $_blob ---"
    else
        echo "--- guest has no Mesa and no Mali blob either ---"
    fi
    return 0
}
