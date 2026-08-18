# Sourced by the launchers in this directory — not executed on its own.
#
# Resolves the QEMU binary and the architecture-dependent half of the command
# line, so one launcher per device family can serve both architectures. ARCH is
# exported by run_instance.sh from the instance's instance.env, where
# new_instance.sh recorded what it read off the built rootfs. It defaults to
# arm64, so a launcher run standalone behaves exactly as it did before.
#
# This used to be a much wider split: armhf attached everything over virtio-mmio,
# because on the 32-bit `virt` machine the PCI host bridge failed to probe at all —
#
#   pci-host-generic 4010000000.pcie: probe with driver pci-host-generic failed
#   with error -75
#
# — which made every `-pci` device invisible in the guest, USB controllers included.
# That is a memory-map problem, not a missing driver, and `highmem=off` fixes it
# (see the armhf branch below), so both architectures now use the same PCI devices.
# What still differs is the machine's highmem flag, the amount of RAM that flag
# allows, the CPU, and the accelerator.

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
        GPU_DEV="virtio-gpu-pci,edid=off,xres=1280,yres=800"
        GPU_GL_DEV="virtio-gpu-gl-pci,edid=off,xres=1280,yres=800"
        INPUT_DEVS="-device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet"
        NET_DEV="virtio-net-pci"
        ;;
    armhf)
        # highmem=off is what makes PCI work here, and nothing else in this branch
        # is possible without it.
        #
        # With the default highmem=on, QEMU puts the PCIe ECAM at 0x40_10000000 and
        # a prefetchable window at 0x80_00000000. linux-image-armmp is the non-LPAE
        # flavour (`# CONFIG_ARM_LPAE is not set`), so resource_size_t is 32 bits
        # and both addresses overflow it while the host bridge is still parsing its
        # `ranges` — that is the -75 in the header, which is -EOVERFLOW, and it
        # happens before any device is probed. Turning highmem off moves the ECAM
        # to 0x3f000000 and the MMIO window to 0x10000000-0x3efeffff, both inside 32
        # bits, and enumeration then works exactly as it does on arm64:
        #
        #   pci-host-generic 3f000000.pcie: ECAM at [mem 0x3f000000-0x3fffffff] ...
        #   virtio-pci 0000:00:01.0: enabling device (0100 -> 0103)
        #   input: QEMU QEMU USB Tablet as /devices/platform/3f000000.pcie/...
        #   [drm] pci: virtio-gpu-pci detected at 0000:00:04.0
        #
        # The kernel needs nothing added: armmp already ships CONFIG_VIRTIO_PCI=y
        # (built in, not a module), CONFIG_PCI_HOST_GENERIC=y and CONFIG_PCI_MSI=y.
        # The other way out would be linux-image-armmp-lpae, which sets
        # CONFIG_PHYS_ADDR_T_64BIT=y and so can keep highmem=on and >3GB of RAM;
        # it is a bigger change than this one and these devices have 2GB anyway.
        MACHINE="virt,highmem=off"
        ARCH_CPU_DEFAULT="cortex-a15"
        GPU_DEV="virtio-gpu-pci,edid=off,xres=1280,yres=800"
        # Still no GL. virtio-gpu-gl-pci is attachable now that the bus works, but
        # virgl has never been tried against a 32-bit guest here, so the virgl
        # display modes keep refusing armhf — they check for an empty value and say
        # so — rather than failing in some less obvious way partway through boot.
        GPU_GL_DEV=""
        INPUT_DEVS="-device usb-ehci -device qemu-xhci,id=xhci -device usb-kbd -device usb-tablet"
        NET_DEV="virtio-net-pci"
        # highmem=off caps the guest's physical address space at 32 bits, and RAM on
        # this machine starts at 0x40000000, so 3072 is the ceiling — above it QEMU
        # refuses to start with "Addressing limited to 32 bits, but memory exceeds
        # it by ...", which does not obviously point back here. Set before
        # run_qemu.sh's own ${MEM:-4096}, which then leaves this value in place.
        MEM="${MEM:-2048}"
        case "$MEM" in
            ''|*[!0-9]*) ;;  # a suffixed value like 2G: leave it for QEMU to judge
            *) [ "$MEM" -le 3072 ] || {
                   echo "ERROR: MEM=$MEM exceeds the 3072 ceiling a 32-bit guest has" >&2
                   echo "       under machine 'virt,highmem=off'. Lower it, or move to" >&2
                   echo "       the armmp-lpae kernel and drop highmem=off." >&2
                   exit 1; } ;;
        esac
        # A 32-bit guest *can* be KVM-accelerated, on a host whose cores implement
        # AArch32 at EL1 -- which is most arm64 Linux hardware, Cortex-A53/A72
        # included. Two things make that easy to miss. It does not work through
        # qemu-system-arm, which distributions build TCG-only, so `-accel help`
        # there lists tcg alone and reads like a hardware limit when it is a
        # packaging one. And the CPU has to be `host,aarch64=off` rather than plain
        # `host`, because what runs the guest is qemu-system-aarch64 with 64-bit
        # execution switched off.
        #
        # Apple Silicon stays on TCG deliberately: M-series cores dropped AArch32
        # entirely, so HVF cannot run a 32-bit guest there at all.
        #
        # Asked of QEMU rather than inferred from uname, because which binary can
        # use KVM is a packaging decision and distributions differ. `-accel help`
        # answers it exactly: QEMU only compiles KVM into a target when the host
        # can host it, so the same qemu-system-aarch64 lists "kvm tcg" on an arm64
        # host and "tcg" alone on x86_64 -- verified on both. That also means a
        # distribution shipping a KVM-enabled qemu-system-arm is picked up here
        # without this needing to know about it.
        #
        # /dev/kvm is checked too: the binary advertising KVM says nothing about
        # whether this kernel exposes a usable device (containers and hosts with
        # virtualization disabled do not).
        #
        # The CPU model comes from the target the binary runs, which its name
        # states outright. A 64-bit target needs AArch64 explicitly switched off to
        # give a 32-bit vCPU; a 32-bit target is already 32-bit and has no such
        # property, so asking for it there fails.
        #
        # Set here rather than in each launcher's host check, which uses
        # ${ACCEL:-...} defaults and so leaves these in place. Both stay overridable
        # from the environment.
        _kvm_ok=0
        if [ "$(uname -s)" = Linux ] &&
           "$QEMU_BIN" -accel help 2>/dev/null | grep -qw kvm &&
           [ -w /dev/kvm ] && [ -r /dev/kvm ]; then
            _kvm_ok=1
        fi
        if [ "$_kvm_ok" = 1 ]; then
            ACCEL="${ACCEL:-kvm}"
            case "${QEMU_BIN##*/}" in
                *aarch64*) CPU="${CPU:-host,aarch64=off}" ;;
                *)         CPU="${CPU:-host}" ;;
            esac
        else
            ACCEL="${ACCEL:-tcg}"
            CPU="${CPU:-cortex-a15}"
        fi
        # One case this cannot see: an arm64 core that implements no AArch32 at all
        # still advertises KVM, and a 32-bit guest on it fails at startup instead of
        # being caught here. Graviton, Ampere Altra and Apple Silicon are all like
        # this. There is no cheap way to ask -- KVM_CAP_ARM_EL1_32BIT is not exposed
        # through any file -- so the failure is left to QEMU, which says so plainly.
        # ACCEL=tcg is the way past it.
        ;;
    *)
        echo "ERROR: unsupported ARCH '$ARCH' (expected arm64 or armhf)." >&2
        exit 1 ;;
esac

# Extra scanouts, for the one product that needs them.
#
# Every Engine device ships a single display except JP22, which declares three in
# its ScreenConfiguration.json. virtio-gpu advertises one connector per scanout,
# so a one-output GPU gives Qt one screen no matter what that file asks for, and
# Engine's second window onto it aborts the process (docs/BUILDING.md).
#
# Left at 1 by default rather than raised for everyone: each scanout is another
# QemuConsole, and how a display backend surfaces the extra ones varies (cocoa
# has no multi-head window management to speak of), so a device that only ever
# had one screen should not grow two dormant ones. Set GPU_MAX_OUTPUTS=3 in an
# instance.env to opt that instance in.

# Referenced by the per-head tablets' display= property below.
GPU_ID="qengine-gpu"
GPU_MAX_OUTPUTS="${GPU_MAX_OUTPUTS:-1}"
case "$GPU_MAX_OUTPUTS" in
    ''|*[!0-9]*)
        echo "ERROR: GPU_MAX_OUTPUTS must be a positive integer, got '$GPU_MAX_OUTPUTS'." >&2
        exit 1 ;;
esac
if [ "$GPU_MAX_OUTPUTS" -gt 1 ]; then
    # No architecture check: this was arm64-only because armhf had the mmio
    # virtio-gpu and no USB to hang per-head tablets off. Both architectures now
    # have virtio-gpu-pci and xhci, so the block below applies to either — though
    # only arm64 has actually been run multi-head.
    #
    # The GPU needs an id so the tablets below can name it as their display.
    GPU_DEV="$GPU_DEV,id=$GPU_ID,max_outputs=$GPU_MAX_OUTPUTS"
    [ -n "$GPU_GL_DEV" ] && GPU_GL_DEV="$GPU_GL_DEV,id=$GPU_ID,max_outputs=$GPU_MAX_OUTPUTS"

    # One absolute pointing device per head, each bound to its own scanout.
    #
    # A single usb-tablet cannot serve several windows: whichever window is
    # clicked, the guest sees one device reporting coordinates in one space, with
    # nothing to say which screen they belong to. QEMU's display=/head= properties
    # bind an input device to a specific scanout, so N tablets give the guest N
    # distinguishable evdev sources — which is what lets one touchbridge instance
    # per head map clicks onto the right screen.
    _tablets=""
    _head=0
    while [ "$_head" -lt "$GPU_MAX_OUTPUTS" ]; do
        _tablets="$_tablets -device usb-tablet,display=$GPU_ID,head=$_head"
        _head=$((_head + 1))
    done

    # Replace the single default tablet rather than adding to it, so head 0 does
    # not end up with two. Checked rather than assumed: a silent no-op here would
    # leave an unbound tablet that quietly steals events from head 0.
    _input_base="${INPUT_DEVS% -device usb-tablet}"
    if [ "$_input_base" = "$INPUT_DEVS" ]; then
        echo "ERROR: expected INPUT_DEVS to end with '-device usb-tablet' so it could" >&2
        echo "       be replaced by per-head tablets; it is: $INPUT_DEVS" >&2
        exit 1
    fi
    INPUT_DEVS="$_input_base$_tablets"
fi

# Engine wants a playback-only card: a capture PCM makes it assign capture as the
# default and leave playback null, which presents as a stuck XRUN rather than an
# error (see the note in each engine launcher).
#
# hda is PCI, which is why armhf used to get the mmio virtio-sound device instead.
# It no longer has to, and it should not: Engine's ALSA shim spoofs a specific card
# name and was only ever written against hda, so the shared card is the pairing more
# likely to work. Neither has been run on armhf yet — if hda turns out to be the
# wrong bet there, virtio-sound-device is still reachable through QEMU_EXTRA_ARGS.
# Kept as a function, rather than folded into a variable, because run_qemu.sh calls
# it with the -audiodev id it generated.
#   $1 = the -audiodev id to attach to
arch_audio_devices() {
    printf -- '-device ich9-intel-hda -device hda-output,audiodev=%s' "$1"
}
