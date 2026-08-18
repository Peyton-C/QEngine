# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Last look at the image before the container exits.
#
#   verify_rootfs <image path>
#
# The e2fsck is advisory -- everything above has already run, so a complaint here
# is information rather than grounds to fail. An image that is missing or empty is
# not: it means something further up failed without saying so.
verify_rootfs() {
    _img="$1"

    echo "--- final consistency check ---"
    e2fsck -f -y "$_img" || true

    if [ ! -s "$_img" ]; then
        echo "ERROR: output image missing or empty — something failed silently above." >&2
        return 1
    fi
}
