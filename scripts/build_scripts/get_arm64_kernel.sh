#!/bin/bash
# get_arm64_kernel.sh — builds a Debian trixie (13) arm64 kernel 
# initrd for booting the RANE SYSTEM ONE / RMZ2 target under QEMU, with the
# modules this project actually needs pre-decompressed and bundled in.
#
# Usage: get_debian_trixie_kernel.sh
#   Writes vmlinuz-generic-arm64 and initrd-generic-arm64 to /build
#   (the repo's build/ directory). Requires Docker.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

echo Getting Kernel...

# Every module this project has needed and previously had to manually
# decompress by hand at some point — see ENGINEOS.md's MIDI section and
# BUILDING.md's Status section for where each group came from — plus the
# DRM/virtio-gpu stack, which mkinitramfs's own MODULES=most policy
# excludes by category the same way it excludes sound/bluetooth.
MODULES=(
    # HID (keyboard/tablet — usb-kbd/usb-tablet under QEMU)
    hid hid_generic usbhid
    # Required for touchbridge_rmz2 to provide the source device
    evdev uinput
    # FAT32 mount
    nls_iso8859-1
    # USB-audio/MIDI class stack (ENGINEOS.md's documented load order)
    snd_hwdep mc snd_seq_device snd_seq snd_rawmidi snd_seq_midi_event
    snd_ump snd_usbmidi_lib snd_seq_midi snd_usb_audio
    # Onboard HDA
    snd snd_timer snd_pcm snd_hda_core snd_hda_codec snd_hda_codec_generic
    snd_intel_dspcfg snd_hda_intel
    # Bluetooth stack
    ecc ecdh_generic bluetooth btintel btrtl btmtk btbcm btusb
    # Display (virtio-gpu-pci under QEMU)
    virtio_gpu drm drm_kms_helper
)

INNER_SCRIPT="$(mktemp /tmp/get-debian-trixie-kernel-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

# Written to a real file and mounted in, not piped via stdin
# (`docker run ... bash -s <<EOF`) — that pattern silently swallows the
# container's stdout under this host's Docker setup (reproducible: even a
# bare `echo` never showed up, despite the command exiting 0). Mounting
# and executing a real file behaves normally.
cat > "$INNER_SCRIPT" <<DOCKER_SCRIPT
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq initramfs-tools linux-image-arm64 >/dev/null 2>&1

KVER=\$(ls /lib/modules/)
echo "Kernel version: \$KVER"

# copymods relocates the initrd's own /lib/modules/<kver> into a tmpfs on
# the real root at boot — Ubuntu-cloud-specific (Scott Moser/Canonical),
# Debian doesn't ship it, so inject the same script content here.
mkdir -p /etc/initramfs-tools/scripts/init-bottom
cat > /etc/initramfs-tools/scripts/init-bottom/copymods <<'EOF'
#!/bin/sh
prereqs() {
	local o="/scripts/init-bottom/overlayroot"  p=""
	for p in "\$DESTDIR/" ""; do
		[ -e "\$p\$o" ] && echo "overlayroot" && return 0
	done
}
[ "\$1" != "prereqs" ] || { prereqs; exit; }
. /scripts/functions
set -f
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cmdline=""
myopts=""
if [ -f /proc/cmdline ]; then
	read cmdline < /proc/cmdline
	for tok in \$cmdline; do
		[ "\${tok#copymods=}" != "\$tok" ] || continue
		myopts="\${tok#copymods=}"
	done
fi
myver=\$(uname -r)
if [ ! -d "/lib/modules/\$myver" ]; then
	log_warning_msg "Something odd, no /lib/modules/\$myver in initramfs."
	exit 0
fi
[ -d "\$rootmnt/lib/modules" ] || mkdir -p "\$rootmnt/lib/modules" ||
	{ log_warning_msg "No /lib/modules in target. cannot help."; exit 0; }
if [ -d "\$rootmnt/lib/modules/\$myver" ]; then
	if [ "\${myopts#*force}" = "\$myopts" ]; then
		exit 0
	else
		log_warning_msg "copying over existing modules! due to copymods=force"
	fi
fi
mount -t tmpfs copymods "\$rootmnt/lib/modules" ||
	{ log_failure_msg "failed mount of tmpfs"; exit 0; }
mv "/lib/modules/\$myver" "\$rootmnt/lib/modules" ||
	{ log_failure_msg "failed to copy modules to target root"; exit 0; }
ln -s "\$rootmnt/lib/modules/\$myver" "/lib/modules/\$myver" ||
	{ log_failure_msg "failed to link to modules"; exit 0; }
EOF
chmod +x /etc/initramfs-tools/scripts/init-bottom/copymods

sed -i 's/^MODULES=.*/MODULES=most/' /etc/initramfs-tools/initramfs.conf

# MODULES=most still excludes whole categories (sound, bluetooth, most of
# drm) by design — force these specific ones in regardless. One name per
# line: /etc/initramfs-tools/modules requires it, and a plain unquoted
# \${MODULES[@]} here would collapse the whole array onto a single
# space-joined line instead (confirmed directly — that silently dropped
# every module that wasn't already pulled in some other way, since
# initramfs-tools then read the whole line as one bogus module name).
printf '%s\n' ${MODULES[@]} >> /etc/initramfs-tools/modules

echo "--- building initrd ---"
mkinitramfs -o /tmp/initrd-generic-arm64 "\$KVER"

echo "--- verifying target modules made it in ---"
mkdir -p /tmp/verify
unmkinitramfs /tmp/initrd-generic-arm64 /tmp/verify >/dev/null 2>&1
MISSING=""
for name in ${MODULES[@]}; do
    # ecc: compiled directly into this kernel (modules.builtin), not a
    #   loadable .ko at all — already active, expected to "fail" here.
    # snd_ump: genuinely absent from this kernel build. Universal MIDI
    #   Packet / MIDI 2.0 support, not needed for the MC6000MK2's classic
    #   USB MIDI 1.0 class-compliant interface (snd_seq_midi/
    #   snd_usbmidi_lib handle that fine without it) — expected too.
    case "\$name" in
        ecc|snd_ump) continue ;;
    esac
    pattern=\$(echo "\$name" | sed 's/[_-]/[-_]/g')
    if ! find /tmp/verify -regextype posix-extended -regex ".*/\${pattern}\.ko(\.xz)?" | grep -q .; then
        MISSING="\$MISSING \$name"
    fi
done
if [ -n "\$MISSING" ]; then
    echo "WARNING: these modules didn't make it into the built initrd:\$MISSING" >&2
fi

cp /tmp/initrd-generic-arm64 /out/initrd-generic-arm64
cp /boot/vmlinuz-"\$KVER" /out/vmlinuz-generic-arm64
if [ ! -s /out/initrd-generic-arm64 ] || [ ! -s /out/vmlinuz-generic-arm64 ]; then
    echo "ERROR: output file(s) missing or empty" >&2
    exit 1
fi
echo "--- done: kernel \$KVER ---"
DOCKER_SCRIPT

docker run --rm --platform linux/arm64 \
    -v "$OUT_DIR:/out" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:trixie bash /inner.sh

if [ ! -s "$OUT_DIR/initrd-generic-arm64" ] || [ ! -s "$OUT_DIR/vmlinuz-generic-arm64" ]; then
    echo "FAILED: expected output files are missing from $OUT_DIR" >&2
    exit 1
fi

echo ""
echo "Built: $OUT_DIR/vmlinuz-generic-arm64"
echo "       $OUT_DIR/initrd-generic-arm64"