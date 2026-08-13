#!/bin/bash
# get_armv7_kernel.sh — builds a Debian trixie (13) armhf kernel 
# initrd for booting the armv7/RK3288 targets (Akai MPC) under QEMU, with the
# modules this project actually needs pre-decompressed and bundled in.
#
# Usage: get_armv7_kernel.sh
#   Writes vmlinuz-generic-armhf and initrd-generic-armhf to /build
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
    # /etc and /var are overlayfs mounts on these images; without this the
    # etc.mount/var.mount units fail and the boot cascades into failures.
    overlay
    # HID (keyboard/tablet — usb-kbd/usb-tablet under QEMU)
    hid hid_generic usbhid
    # Built into the arm64 kernel but modules on armhf, so they have to be named
    # here: without virtio_blk the root device never appears and the initramfs
    # gives up with "ALERT! UUID=... does not exist".
    virtio_blk virtio_net
    # The 32-bit virt machine has no PCI, so the USB controllers are unreachable
    # and virtio input devices are the only way to get a pointer in.
    virtio_input
    # Required for touchbridge_rmz2 to provide the source device
    evdev uinput
    # FAT32/exFAT mount (USB flash drive with a real Engine Library).
    # fat is the shared core, vfat the long-filename driver; nls_cp437 is
    # the codepage vfat asks for by default at mount time, and its absence
    # fails the mount even when the driver itself is loaded.
    fat vfat exfat nls_cp437 nls_iso8859-1 nls_ascii
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
# A wrong-architecture container would silently produce an arm64 kernel under an
# armhf name, which fails much later and confusingly at boot.
case "\$(uname -m)" in armv7l|armv8l|armhf) ;; *)
    echo "ERROR: container is \$(uname -m), expected armv7l." >&2; exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq initramfs-tools linux-image-armmp >/dev/null 2>&1

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
mkinitramfs -o /tmp/initrd-generic-armhf "\$KVER"

echo "--- verifying target modules made it in ---"
mkdir -p /tmp/verify
unmkinitramfs /tmp/initrd-generic-armhf /tmp/verify >/dev/null 2>&1
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

cp /tmp/initrd-generic-armhf /out/initrd-generic-armhf
cp /boot/vmlinuz-"\$KVER" /out/vmlinuz-generic-armhf
if [ ! -s /out/initrd-generic-armhf ] || [ ! -s /out/vmlinuz-generic-armhf ]; then
    echo "ERROR: output file(s) missing or empty" >&2
    exit 1
fi
echo "--- done: kernel \$KVER ---"
DOCKER_SCRIPT

# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform linux/arm/v7 debian:trixie >/dev/null
docker run --rm --platform linux/arm/v7 \
    -v "$OUT_DIR:/out" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:trixie bash /inner.sh

if [ ! -s "$OUT_DIR/initrd-generic-armhf" ] || [ ! -s "$OUT_DIR/vmlinuz-generic-armhf" ]; then
    echo "FAILED: expected output files are missing from $OUT_DIR" >&2
    exit 1
fi

echo ""
echo "Built: $OUT_DIR/vmlinuz-generic-armhf"
echo "       $OUT_DIR/initrd-generic-armhf"