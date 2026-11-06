# Sourced by the build scripts in this directory, not executed on its own.
#
# Pulls the root filesystem image out of an inMusic firmware update. Both rootfs
# builders started from the same code here and had drifted into two byte-identical
# copies; new_instance.sh needs the same thing again to identify a firmware's device
# family before it can pick a builder, which would have made three.
#
#   extract_rootfs <firmware image> <destination path>
#
# On success the destination holds a raw ext2/3/4 image and EXTRACTED_ROOTFS_SIZE is
# set to its size in bytes. The caller owns the destination.
#
# Each container inMusic ships has a reader beside this file, picked by the first four
# bytes. Each parses its container, checks the rootfs digest it carries and streams the
# decompression, so nothing is carved and no scratch directory is needed. They replace
# binwalk, which could not be trusted with any of the three: binwalk 3 claims a plain
# FIT as one devicetree and never reaches the rootfs inside it, it only carves AZ0x,
# expanding a 474 MB Prime 4+ image to ~8 GB of scratch, and binwalk 2 glues the boot
# partition onto the rootfs on any AZ0x image where boot precedes it, as on SYSTEM ONE
# 5.2.0, which then passes for a valid ext image.
#
# BINWALK=1 goes back to binwalk anyway, for a container no reader knows yet or to
# check a reader against it. Nothing is verified on that path.
_EXTRACT_ROOTFS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

extract_rootfs() {
    _firmware="$1"
    _dest="$2"

    if [ "${BINWALK:-0}" = 1 ]; then
        _extract_rootfs_binwalk "$_firmware" "$_dest"
        return
    fi

    _magic="$(od -An -tx1 -N4 "$_firmware" | tr -d ' \n')"
    case "$_magic" in
        d00dfeed) _reader="extract_fit_rootfs.py" ;;
        415a3031) _reader="extract_az01_rootfs.py" ;;
        415a3078) _reader="extract_az0x_rootfs.py" ;;
        *)
            echo "ERROR: $_firmware is not a firmware container QEngine reads (magic '$_magic')." >&2
            echo "       Known: plain FIT (d00dfeed), AZ01 (415a3031) and AZ0x (415a3078)." >&2
            echo "       BINWALK=1 (--binwalk on new_instance.sh) tries binwalk instead." >&2
            return 1 ;;
    esac

    echo "--- extracting $_firmware with $_reader ---"
    if ! EXTRACTED_ROOTFS_SIZE="$(python3 -I "$_EXTRACT_ROOTFS_DIR/$_reader" "$_firmware" "$_dest")"; then
        echo "ERROR: $_reader failed on $_firmware." >&2
        return 1
    fi
    echo "--- extracted verified rootfs ($((EXTRACTED_ROOTFS_SIZE / 1024 / 1024)) MiB) ---"
}

_extract_rootfs_binwalk() {
    command -v binwalk >/dev/null 2>&1 || {
        echo "ERROR: BINWALK=1 was given but 'binwalk' is not on PATH." >&2
        return 1; }
    _scratch="$(mktemp -d /tmp/qengine-extract.XXXXXX)"

    echo "--- extracting $1 with binwalk (this scans the whole image, ~10s+) ---"
    if ! binwalk -e -C "$_scratch" "$1"; then
        rm -rf "$_scratch"
        echo "ERROR: binwalk failed on $1." >&2
        return 1
    fi

    # binwalk signature-scans rather than parsing the container, so it finds every
    # embedded ext2/3/4 filesystem: the rootfs plus smaller boot-slot partitions.
    # The largest is taken to be the rootfs.
    _best="" _best_size=0
    while IFS= read -r -d '' f; do
        if file "$f" | grep -q 'ext[234] filesystem'; then
            f_size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
            if [ "$f_size" -gt "$_best_size" ]; then
                _best="$f" _best_size="$f_size"
            fi
        fi
    done < <(find "$_scratch" -type f -print0)

    if [ -z "$_best" ]; then
        rm -rf "$_scratch"
        echo "ERROR: no ext2/3/4 filesystem image found in binwalk's extraction output." >&2
        return 1
    fi

    echo "--- found rootfs candidate: $_best ($((_best_size / 1024 / 1024)) MiB, unverified) ---"
    cp "$_best" "$2"
    EXTRACTED_ROOTFS_SIZE="$_best_size"

    # Freed here rather than in an EXIT trap: the callers install their own traps for
    # loop devices and mount points, and a trap set here would replace one of theirs.
    rm -rf "$_scratch"
}
