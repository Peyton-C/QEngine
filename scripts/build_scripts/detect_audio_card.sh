# Sourced by the build scripts in this directory — not executed on its own.
#
# Reports the ALSA card name a product's real hardware registers, so alsashim can
# spoof the emulated card as that name and get past Engine's compiled-in
# card-name allowlist.
#
#   detect_audio_card <rootfs image> <product code>
#
# On success AUDIO_CARD_NAME is set.
#
# Why this is the value that matters: Engine's ALSADeviceEnumerator::scanDevices()
# rejects any card whose snd_ctl_card_info_get_name() is not in the accepted-name
# vector built from its per-product config, before it ever looks at the card's PCM
# devices — so under QEMU the emulated card is skipped on its name alone. See
# shims/alsashim/alsashim.c and docs/ENGINEOS.md.
#
# Why detect rather than tabulate. The name is the ASoC card name the vendor's
# machine driver registers, and on RK3288 that is *not* reliably the product code.
# snd-soc-rockchip-inmusic-jp07.ko carries one entry per variant — a devicetree
# compatible, a DAI link name and a card name ("jp07", "JP07 PCM", "JP07") — and
# several products share one entry. JP08 is exactly that case: its devicetree
# declares
#
#     compatible = "inmusic,jp08-audio", "inmusic,jp07-audio";
#
# and the driver matches nothing named jp08, so it binds on the *second* string
# and the card comes up as "JP07". A table keyed to the product code would spoof
# "JP08" there and be rejected in the same silent way an unspoofed card is.
#
# So both halves are read off the rootfs being built: the devicetree says which
# compatibles this product asks for and in what order, and the module's own alias
# list says which of them a driver will actually answer. The first compatible that
# is claimed is the one that binds, and its uppercased device token is the card
# name. That is the same rule the kernel applies at probe time.
#
# RK3588 needs none of this — RMZ2 uses simple-audio-card with
# `simple-audio-card,name = "RMZ2"` written straight into its devicetree, which is
# why alsashim's built-in default is that string and why build_arm64_rootfs.sh
# does not call this.
detect_audio_card() {
    _img="$1"
    _product="$2"
    AUDIO_CARD_NAME=""

    # tr rather than ${_product,,}: this is sourced by /bin/sh-compatible builders.
    _lower="$(printf '%s' "$_product" | tr '[:upper:]' '[:lower:]')"

    # Same host-tool lookup as detect_mesa.sh: e2fsprogs installs into /sbin on
    # Debian, and Homebrew keeps it off PATH entirely.
    _dbg=""
    for _c in debugfs /sbin/debugfs /usr/sbin/debugfs \
              /opt/homebrew/opt/e2fsprogs/sbin/debugfs; do
        if command -v "$_c" >/dev/null 2>&1; then _dbg="$_c"; break; fi
    done
    [ -n "$_dbg" ] || {
        echo "ERROR: no debugfs found; install e2fsprogs." >&2; return 1; }

    # Falling back to the product code rather than failing the build: it is the
    # right answer for every RK3288 product whose devicetree names its own code
    # (all of them except JP08), it is what alsashim would have to be told by hand
    # anyway, and a firmware laid out differently enough to get here is not a
    # reason to refuse to produce a rootfs. Each path below says which check it
    # was that could not be made, so the guess is never silent.
    _audio_card_giveup() {
        echo "--- audio card name: assuming \"$_product\" ($1) ---" >&2
        AUDIO_CARD_NAME="$_product"
        return 0
    }

    # `|| true` on each pipeline below: the builders run with `set -e` and
    # `set -o pipefail`, under which a grep that matches nothing — an ordinary
    # outcome here, and one this function handles on the next line — would abort
    # the whole build from inside the assignment instead.
    #
    # The per-product devicetree, e.g. /boot/rk3288-az01-jp07.dtb. Several
    # board-revision variants of one product ship alongside it (-c, -revf); they
    # agree on the sound node, so the plainest name — the shortest — is taken.
    _dtb="$("$_dbg" -R 'ls -p /boot' "$_img" 2>/dev/null |
        awk -F/ -v p="-$_lower" '
            $6 ~ /\.dtb$/ && index($6, p) &&
            (best == "" || length($6) < length(best)) { best = $6 }
            END { print best }' || true)"
    [ -n "$_dtb" ] || { _audio_card_giveup "no /boot/*$_lower*.dtb in this rootfs"; return 0; }

    _tmp="$(mktemp -d /tmp/qengine-audio-card.XXXXXX)"
    "$_dbg" -R "dump /boot/$_dtb $_tmp/product.dtb" "$_img" >/dev/null 2>&1

    # A devicetree blob stores property values inline in its structure block, in
    # node order, so matching the raw file yields this product's compatibles in
    # the order the kernel would try them. -a because the blob is binary; the
    # -codec alternative is matched only so that the codec node's
    # "inmusic,jp07-audio-codec" is recognised and dropped rather than being
    # truncated to a bogus audio-node match by the shorter pattern.
    _compatibles="$(grep -aoE 'inmusic,[a-z0-9]+-audio(-codec)?' "$_tmp/product.dtb" 2>/dev/null |
        grep -v -- '-codec$' || true)"
    [ -n "$_compatibles" ] || {
        rm -rf "$_tmp"
        _audio_card_giveup "no inmusic,*-audio compatible in $_dtb"; return 0; }

    # Which of them a driver in this rootfs will actually bind. modules.alias is
    # the map depmod already built from every module's MODULE_DEVICE_TABLE, so it
    # answers for the whole tree without unpacking a single .ko.
    _kver="$("$_dbg" -R 'ls -p /usr/lib/modules' "$_img" 2>/dev/null |
        awk -F/ '$6 != "." && $6 != ".." && !seen++ { print $6 }' || true)"
    if [ -n "$_kver" ]; then
        "$_dbg" -R "dump /usr/lib/modules/$_kver/modules.alias $_tmp/modules.alias" \
            "$_img" >/dev/null 2>&1
    fi

    _match=""
    if [ -s "$_tmp/modules.alias" ]; then
        for _cand in $_compatibles; do
            # depmod writes one alias line per compatible, as
            # `alias of:N*T*C<compatible>[C*] <module>`. The trailing delimiter
            # is what keeps a prefix from matching: without it "inmusic,jp07-audio"
            # would also be found inside "inmusic,jp07-audio-codec", i.e. the
            # codec driver's alias would be read as the machine driver's.
            if grep -q "^alias of:.*C${_cand} " "$_tmp/modules.alias" ||
               grep -q "^alias of:.*C${_cand}C\* " "$_tmp/modules.alias"; then
                _match="$_cand"
                break
            fi
        done
        [ -n "$_match" ] || {
            rm -rf "$_tmp"
            _audio_card_giveup "no driver in this rootfs claims any of: $(echo $_compatibles)"
            return 0; }
    else
        # No module map to consult. The first compatible is the kernel's own first
        # choice, so it is the best available guess — and it is only wrong for a
        # product like JP08 that lists a compatible nothing implements.
        _match="$(printf '%s\n' "$_compatibles" | awk 'NR == 1')"
        echo "--- no modules.alias in this rootfs; taking the devicetree's first" \
             "audio compatible unverified ---" >&2
    fi
    rm -rf "$_tmp"

    # "inmusic,jp07-audio" -> "JP07", the ASoC card name that driver registers.
    _token="${_match#inmusic,}"
    _token="${_token%-audio}"
    AUDIO_CARD_NAME="$(printf '%s' "$_token" | tr '[:lower:]' '[:upper:]')"

    if [ "$AUDIO_CARD_NAME" = "$_product" ]; then
        echo "--- audio card name: $AUDIO_CARD_NAME (from $_dtb) ---"
    else
        echo "--- audio card name: $AUDIO_CARD_NAME — $_product shares" \
             "$_match with it (from $_dtb) ---"
    fi
    return 0
}
