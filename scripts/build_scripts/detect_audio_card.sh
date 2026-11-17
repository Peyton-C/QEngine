# shellcheck shell=bash
# Sourced by the build scripts in this directory — not executed on its own.
#
# Reports the ALSA card name a product's real hardware registers, so alsashim can
# spoof the emulated card as that name and get past Engine's compiled-in
# card-name allowlist.
#
#   detect_audio_card <rootfs image> <product code> [firmware image]
#
# The firmware is read only when the rootfs has no devicetree for the product.
# Where neither has one, the name comes from the table in _audio_card_known below.
# A name already in AUDIO_CARD_NAME (new_instance.sh --audio-card-name) is kept and
# nothing is detected.
#
# On success AUDIO_CARD_NAME is set.
#
# Why this is the value that matters: Engine's ALSADeviceEnumerator::scanDevices()
# rejects any card whose snd_ctl_card_info_get_name() is not in the accepted-name
# vector built from its per-product config, before it ever looks at the card's PCM
# devices — so under QEMU the emulated card is skipped on its name alone. See
# shims/alsashim/alsashim.c and docs/engine/AUDIO_MIDI.md.
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
_DETECT_AUDIO_CARD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The card name each product is known to get, for when its devicetree is not in the
# image being built. inMusic splits each release into images by family, and each
# carries only its own family's devicetrees: the 5.3.0 Prime 4 image has JC11, JC16
# and NH08's, the SC6000 image JP13 and JP14's, while Engine itself is identical in
# all of them. So building SC5000M from an SC6000 image boots, but finds no JP08
# devicetree, and without this would spoof "JP08" and be refused in silence.
#
# Only the products whose name is not their code are listed. Every entry was read by
# detect_audio_card itself from every armv7 update image from 2.4.0 to 5.3.0 that
# carries the product's devicetree, and none changed across them. Detection still
# wins wherever it can run, and a detected name that disagrees with this table is
# reported, so a rename in a later firmware shows up at build time rather than as
# an instance with no sound.
_audio_card_known() {
    case "$1" in
        # The XMOS USB interface; see the JC11 case in detect_audio_card.
        JC11|JC11S)     printf '%s' "PRIME 4 Audio" ;;
        # Their devicetrees list their own -audio compatible first, which no
        # driver claims, then the one the shared driver binds.
        JP08|JP13|JP14) printf '%s' JP07 ;;
        JP11S)          printf '%s' JP11 ;;
        NH08S)          printf '%s' NH08 ;;
        *)              printf '%s' "$1" ;;
    esac
}

detect_audio_card() {
    _img="$1"
    _product="$2"
    _firmware="${3:-}"

    if [ -n "${AUDIO_CARD_NAME:-}" ]; then
        echo "--- audio card name: $AUDIO_CARD_NAME (given, not detected) ---"
        return 0
    fi
    AUDIO_CARD_NAME=""
    _known="$(_audio_card_known "$_product")"

    # Prime 4 and Prime 4+ are the exception to all of the above: their audio is
    # an XMOS USB interface rather than an ASoC card, the devicetree has no
    # inmusic,*-audio node to read, and the name Engine waits for is the
    # interface's USB product string. JC11 5.3.0 never opens a card named "JC11"
    # and plays on one named this in the Pi chroot; JC11S 5.0.4 opened both of
    # its PCMs under this name in QEMU.
    case "$_product" in
        JC11|JC11S)
            AUDIO_CARD_NAME="$_known"
            echo "--- audio card name: $AUDIO_CARD_NAME (XMOS USB interface) ---"
            return 0 ;;
    esac

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

    # Falling back to the table, and past it to the product code, rather than
    # failing the build: a firmware laid out differently enough to get here is not
    # a reason to refuse to produce a rootfs. Each path below says which check it
    # was that could not be made, so the fallback is never silent.
    _audio_card_giveup() {
        echo "--- audio card name: taking \"$_known\" from the table ($1) ---" >&2
        AUDIO_CARD_NAME="$_known"
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

    _tmp="$(mktemp -d /tmp/qengine-audio-card.XXXXXX)"
    if [ -n "$_dtb" ]; then
        "$_dbg" -R "dump /boot/$_dtb $_tmp/product.dtb" "$_img" >/dev/null 2>&1
    elif [ -n "$_firmware" ] &&
         python3 -I "$_DETECT_AUDIO_CARD_DIR/extract_dtb.py" "$_firmware" "$_product" \
             "$_tmp/product.dtb"; then
        # The AZ05 generation (JP11S from 5.x, JP20, JP21, NH08S, NH10) keeps its
        # devicetrees in the update's boot partition rather than the rootfs. Without
        # this NH08S would guess "NH08S", while its driver registers "NH08".
        _dtb="the firmware's $_lower devicetree"
    else
        rm -rf "$_tmp"
        _audio_card_giveup "no /boot/*$_lower*.dtb in this rootfs or the firmware"; return 0
    fi

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
    if [ "$AUDIO_CARD_NAME" != "$_known" ]; then
        echo "WARNING: detected \"$AUDIO_CARD_NAME\" for $_product, but _audio_card_known" \
             "in detect_audio_card.sh has \"$_known\". Using the detected name; update" \
             "the table, or a build of $_product from another image will be wrong." >&2
    fi
    return 0
}
