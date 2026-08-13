# Sourced by the launchers in this directory — not executed on its own.
#
# Resolves the QEMU binary and the architecture-dependent half of the command
# line, so one launcher per device family can serve both architectures. ARCH is
# exported by run_instance.sh from the instance's instance.env, where
# new_instance.sh recorded what it read off the built rootfs. It defaults to
# arm64, so a launcher run standalone behaves exactly as it did before.
#
# The split exists because the 32-bit `virt` machine has no usable PCI. The Debian
# armmp kernel's driver fails to probe it:
#
#   pci-host-generic 4010000000.pcie: probe with driver pci-host-generic failed
#   with error -75
#
# so every `-pci` device is invisible in the guest — including the USB controllers
# — and virtio-mmio is the only way to attach anything.

ARCH="${ARCH:-arm64}"

# One binary covers both guests: qemu-system-aarch64 offers cortex-a15/cortex-a7
# and boots a 32-bit zImage (verified against the armv7 MPC rootfs). Prefer it, and
# fall back to qemu-system-arm only for a 32-bit guest on a host that packages just
# the 32-bit build. brew's prefix is not always on PATH.
if [ -z "${QEMU_BIN:-}" ]; then
    for _candidate in qemu-system-aarch64 \
                      /opt/homebrew/bin/qemu-system-aarch64 \
                      /usr/local/bin/qemu-system-aarch64; do
        command -v "$_candidate" >/dev/null 2>&1 && { QEMU_BIN="$_candidate"; break; }
    done
fi
if [ -z "${QEMU_BIN:-}" ] && [ "$ARCH" = armhf ]; then
    for _candidate in qemu-system-arm /opt/homebrew/bin/qemu-system-arm; do
        command -v "$_candidate" >/dev/null 2>&1 && { QEMU_BIN="$_candidate"; break; }
    done
fi
[ -n "${QEMU_BIN:-}" ] || {
    echo "ERROR: no QEMU binary found for $ARCH." >&2
    echo "       Install qemu-system-aarch64 — it runs both 32- and 64-bit ARM" >&2
    echo "       guests — or set QEMU_BIN to the binary to use." >&2
    exit 1; }

case "$ARCH" in
    arm64)
        MACHINE="virt,highmem=on"
        # The default when nothing else picks one. Deliberately not assigned to CPU
        # itself: the launchers' own host check sets CPU=host for KVM/HVF, and a
        # value here would win over it via ${CPU:-...} and silently disable it.
        ARCH_CPU_DEFAULT="max"
        MMIO_GLOBAL=""
        GPU_DEV="virtio-gpu-pci,edid=off,xres=1280,yres=800"
        GPU_GL_DEV="virtio-gpu-gl-pci,edid=off,xres=1280,yres=800"
        INPUT_DEVS="-device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet"
        NET_DEV="virtio-net-pci"
        ;;
    armhf)
        MACHINE="virt"
        ARCH_CPU_DEFAULT="cortex-a15"
        # virtio-gpu requires VIRTIO_F_VERSION_1, which the mmio transport only
        # offers once the legacy interface is turned off. Without this the guest
        # driver refuses the device and there is no display at all.
        MMIO_GLOBAL="-global virtio-mmio.force-legacy=false"
        GPU_DEV="virtio-gpu-device"
        # No GL: virtio-gpu-gl is PCI-only, so the virgl launchers cannot serve
        # this architecture. They check for an empty value and say so.
        GPU_GL_DEV=""
        INPUT_DEVS="-device virtio-keyboard-device -device virtio-tablet-device"
        NET_DEV="virtio-net-device"
        # A 32-bit guest cannot be accelerated by KVM or HVF on the hosts this
        # project targets — Apple Silicon has no AArch32 at all — and `-cpu host`
        # is accelerator-only. Set here rather than in each launcher's host check,
        # which uses ${ACCEL:-...} defaults and so leaves these in place. Both stay
        # overridable from the environment.
        ACCEL="${ACCEL:-tcg}"
        CPU="${CPU:-cortex-a15}"
        ;;
    *)
        echo "ERROR: unsupported ARCH '$ARCH' (expected arm64 or armhf)." >&2
        exit 1 ;;
esac

# Engine wants a playback-only card: a capture PCM makes it assign capture as the
# default and leave playback null, which presents as a stuck XRUN rather than an
# error (see the note in each engine launcher). hda is PCI, so a 32-bit guest has
# to use the mmio virtio-sound device instead — that pairing has not been tested
# against Engine's ALSA shim, which spoofs a specific card name.
#   $1 = the -audiodev id to attach to
arch_audio_devices() {
    case "$ARCH" in
        arm64) printf -- '-device ich9-intel-hda -device hda-output,audiodev=%s' "$1" ;;
        armhf) printf -- '-device virtio-sound-device,audiodev=%s' "$1" ;;
    esac
}
