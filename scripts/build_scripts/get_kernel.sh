#!/bin/bash
# get_kernel.sh — builds a Debian trixie (13) kernel and initrd for booting this
# project's targets under QEMU, with the modules it actually needs bundled in.
#
# Usage: get_kernel.sh --arch <arm64|armhf>
#   arm64 boots the RK3588 targets (RANE SYSTEM ONE / RMZ2), armhf the RK3288 ones
#   (Akai MPC, and the armv7 Engine OS devices).
#   Writes vmlinuz-generic-<arch> and initrd-generic-<arch> to the repo's build/.
#   Requires Docker.
#
# This was two scripts, get_arm64_kernel.sh and get_armv7_kernel.sh, differing in 48
# lines of 360: the package name, the Docker platform, the output names, the container
# architecture assertion, and three extra modules the armhf kernel needs. Those are
# the six values below; everything else was the same text twice. The old names remain
# as wrappers, since BUILD_ARM64.md and BUILD_MPC.md tell readers to run them.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/build"
mkdir -p "$OUT_DIR"

ARCH=""
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

case "$ARCH" in
    arm64)
        KERNEL_PKG="linux-image-arm64"
        DOCKER_PLATFORM="linux/arm64"
        # Guest architectures QEMU/binfmt may report for this platform.
        EXPECTED_UNAME="aarch64|arm64"
        EXPECTED_NAME="aarch64"
        EXTRA_MODULES=()
        ;;
    armhf)
        KERNEL_PKG="linux-image-armmp"
        DOCKER_PLATFORM="linux/arm/v7"
        EXPECTED_UNAME="armv7l|armv8l|armhf"
        EXPECTED_NAME="armv7l"
        EXTRA_MODULES=(
            # /etc and /var are overlayfs mounts on these images; without this the
            # etc.mount/var.mount units fail and the boot cascades into failures.
            overlay
            # Built into the arm64 kernel but modules on armhf, so they have to be
            # named here: without virtio_blk the root device never appears and the
            # initramfs gives up with "ALERT! UUID=... does not exist".
            virtio_blk virtio_net
            # Kept, but no longer load-bearing. This was the only way to get a
            # pointer into a 32-bit guest back when that machine had no reachable
            # PCI and so no USB controllers; the launchers now run it with
            # highmem=off, which fixes PCI, and the pointer arrives over usb-tablet
            # through the usbhid above. Costs nothing and keeps the mmio input
            # devices usable from QEMU_EXTRA_ARGS.
            virtio_input
        )
        ;;
    *)
        echo "ERROR: --arch must be 'arm64' or 'armhf' (got '${ARCH:-}')." >&2
        exit 1 ;;
esac

VMLINUZ="vmlinuz-generic-$ARCH"
INITRD="initrd-generic-$ARCH"

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
MODULES+=("${EXTRA_MODULES[@]+"${EXTRA_MODULES[@]}"}")

INNER_SCRIPT="$(mktemp /tmp/get-debian-trixie-kernel-inner.XXXXXX.sh)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

# Written to a real file and mounted in, not piped via stdin
# (`docker run ... bash -s <<EOF`) — that pattern silently swallows the
# container's stdout under this host's Docker setup (reproducible: even a
# bare `echo` never showed up, despite the command exiting 0). Mounting
# and executing a real file behaves normally.
cat > "$INNER_SCRIPT" <<DOCKER_SCRIPT
set -euo pipefail
# A wrong-architecture container would silently produce a kernel of the other
# architecture under this one's name, which fails much later and confusingly at boot.
case "\$(uname -m)" in $EXPECTED_UNAME) ;; *)
    echo "ERROR: container is \$(uname -m), expected $EXPECTED_NAME." >&2; exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq initramfs-tools $KERNEL_PKG >/dev/null 2>&1

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
mkinitramfs -o /tmp/$INITRD "\$KVER"

echo "--- verifying target modules made it in ---"
mkdir -p /tmp/verify
unmkinitramfs /tmp/$INITRD /tmp/verify >/dev/null 2>&1
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

cp /tmp/$INITRD /out/$INITRD
cp /boot/vmlinuz-"\$KVER" /out/$VMLINUZ
if [ ! -s /out/$INITRD ] || [ ! -s /out/$VMLINUZ ]; then
    echo "ERROR: output file(s) missing or empty" >&2
    exit 1
fi
echo "--- done: kernel \$KVER ---"
DOCKER_SCRIPT

# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform "$DOCKER_PLATFORM" debian:trixie >/dev/null
docker run --rm --platform "$DOCKER_PLATFORM" \
    -v "$OUT_DIR:/out" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:trixie bash /inner.sh

if [ ! -s "$OUT_DIR/$INITRD" ] || [ ! -s "$OUT_DIR/$VMLINUZ" ]; then
    echo "FAILED: expected output files are missing from $OUT_DIR" >&2
    exit 1
fi

echo ""
echo "Built: $OUT_DIR/$VMLINUZ"
echo "       $OUT_DIR/$INITRD"