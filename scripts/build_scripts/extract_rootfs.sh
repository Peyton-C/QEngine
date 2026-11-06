# Sourced by the build scripts in this directory — not executed on its own.
#
# Pulls the root filesystem image out of an inMusic firmware update. Both rootfs
# builders started from the same code here and had drifted into two byte-identical
# copies; new_instance.sh needs the same thing again to identify a firmware's device
# family before it can pick a builder, which would have made three.
#
#   extract_rootfs <firmware image> <destination path>
#
# On success the destination holds a raw ext2/3/4 image and EXTRACTED_ROOTFS_SIZE is
# set to its size in bytes. The caller owns the destination; the scratch directory
# used along the way is removed before returning.
#
# A container QEngine can parse goes to its own reader, picked by the first four
# bytes, and everything else to binwalk. Plain FIT images (d00dfeed) have to: binwalk 3
# claims the whole file as one devicetree and never reaches the rootfs inside it.
# AZ0x is parsed too: binwalk only carves it, expanding a 474 MB Prime 4+ image to
# ~8 GB of scratch, and binwalk 2 glues the boot partition onto the rootfs on any
# image where boot precedes it, as on SYSTEM ONE 5.2.0.
_EXTRACT_ROOTFS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

extract_rootfs() {
    _firmware="$1"
    _dest="$2"

    _reader=""
    case "$(od -An -tx1 -N4 "$_firmware" | tr -d ' \n')" in
        d00dfeed) _reader="extract_fit_rootfs.py" ;;
        415a3078) _reader="extract_az0x_rootfs.py" ;;
    esac
    if [ -n "$_reader" ]; then
        echo "--- extracting $_firmware with $_reader ---"
        if ! EXTRACTED_ROOTFS_SIZE="$(python3 -I "$_EXTRACT_ROOTFS_DIR/$_reader" "$_firmware" "$_dest")"; then
            echo "ERROR: $_reader failed on $_firmware." >&2
            return 1
        fi
        echo "--- extracted verified rootfs ($((EXTRACTED_ROOTFS_SIZE / 1024 / 1024)) MiB) ---"
        return 0
    fi

    _scratch="$(mktemp -d /tmp/qengine-extract.XXXXXX)"

    echo "--- extracting $_firmware with binwalk (this scans the whole image, ~10s+) ---"
    if ! binwalk -e -C "$_scratch" "$_firmware"; then
        rm -rf "$_scratch"
        echo "ERROR: binwalk failed on $_firmware." >&2
        return 1
    fi

    # binwalk 3 signature-scans rather than parsing the firmware container
    # format, so it finds every embedded ext2/3/4 filesystem — the real rootfs
    # (~830MB) plus two much smaller redundant boot-slot partitions. Identify
    # the rootfs by picking the largest ext2/3/4 image found, rather than
    # hardcoding an offset that's specific to this one firmware build.
    BEST_CANDIDATE=""
    BEST_SIZE=0
    while IFS= read -r -d '' f; do
        if file "$f" | grep -q 'ext[234] filesystem'; then
            f_size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
            if [ "$f_size" -gt "$BEST_SIZE" ]; then
                BEST_CANDIDATE="$f"
                BEST_SIZE="$f_size"
            fi
        fi
    done < <(find "$_scratch" -type f -print0)

    if [ -z "$BEST_CANDIDATE" ]; then
        rm -rf "$_scratch"
        echo "ERROR: no ext2/3/4 filesystem image found in binwalk's extraction output." >&2
        return 1
    fi

    echo "--- found rootfs candidate: $BEST_CANDIDATE ($((BEST_SIZE / 1024 / 1024)) MiB) ---"
    cp "$BEST_CANDIDATE" "$_dest"
    EXTRACTED_ROOTFS_SIZE="$BEST_SIZE"

    # Freed here rather than in an EXIT trap: the callers install their own traps for
    # loop devices and mount points, and a trap set here would replace one of theirs.
    rm -rf "$_scratch"
}
