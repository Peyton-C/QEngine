# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Grows the extracted filesystem to fill the image the builder already resized.
#
#   resize_filesystem <image path>
#
# resize2fs refuses to touch a filesystem that has not been checked, so the
# e2fsck is part of the same step rather than something a caller can forget.
# Both builders do exactly this, which is why it lives here.
resize_filesystem() {
    _img="$1"

    echo "--- e2fsck (required before resize2fs) ---"
    set +e
    e2fsck -f -y "$_img"
    _fsck_rc=$?
    set -e
    # 0 = clean, 1/2 = errors found and corrected — all fine to proceed from.
    # Anything higher means e2fsck couldn't fix it.
    if [ "$_fsck_rc" -gt 2 ]; then
        echo "ERROR: e2fsck failed with exit code $_fsck_rc" >&2
        return 1
    fi

    echo "--- resize2fs ---"
    resize2fs "$_img"
}
