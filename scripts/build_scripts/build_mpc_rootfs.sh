#!/bin/bash
# Automates extraction and modification of a stock Akai MPC rootfs for QEngine.
# Also builds MPC Sample 1.3.0 and HeadRush VX5 1.2.2 on RK3566 (arm64),
# using ac50.service and HV03.service respectively. VX5 uses JUCE/DRM, unlike
# the Qt/EGL Evil application on the RK3288 HeadRush models.
# Covers both generations: Gen 1 on RK3288 (armhf) and Gen 2 on RK3588 (arm64),
# which also carries Force (Gen 2). It also builds the Alesis drum module, whose
# /usr/bin/DrumModule is MPC's JUCE codebase on Gen 1's RK3288 platform, linking
# libdrm and nothing GL the same way, and started from alesis-drummodule.service
# rather than acvs.service. Which app this is gets read off the rootfs. MPC needs far less doing to it than Engine
# does, because it drives KMS directly and links no Mali/EGL, and that holds on
# both: /usr/bin/MPC in 3.10.0 Gen 2 links libdrm and nothing GL, and no libteec,
# so neither drmatomic nor teeshim applies. alsashim does, for sound only.
#
# Steps:
#   1. Extract the rootfs partition out of the firmware image (extract_rootfs.sh).
#   2. Grow the image and its filesystem to a runtime-usable size.
#   3. Block telemetry (docs/BLOCKING_TELEMETRY.md).
#   4. Build touchbridge, dtshim, mpcsurface, alsashim and vx5surface for $ARCH (the shared sources
#      under shims/ are architecture-independent — see the build step below).
#   5. Copy them into /root, wire touchbridge_mpc.service ahead of the app's
#      unit, and preload dtshim into that unit so the app can read a product code.
#      MPC and MPC Sample also get mpcsurface, a virtual control surface, and VX5
#      gets vx5surface, its panel and MIDI port.
#   6. Blank the root password for passwordless serial-console login, and
#      disable the tty1 getty so stray keystrokes can't reach a hidden root
#      shell behind the fullscreen display.
#
# Usage: build_mpc_rootfs.sh [--firmware <path>] [--out <path>]
#                               [--size <bytes>] [--force]
#   --firmware  Firmware .img to extract from.
#   --out       Output rootfs image path. Default: build/rootfs_out.img
#   --size      Final image size in bytes. Default: 4294967296 (4GiB)
#   --force     Overwrite --out if it already exists.
#
# Environment:
#   ARCH          armhf (Gen 1, the default) or arm64 (Gen 2). The same variable
#                 run_qemu.sh reads; new_instance.sh sets it from the firmware.
#   PRODUCT_CODE  which of this image's device identities to spoof. Default ACV5
#                 (MPC X) on armhf, ACVA3 (MPC One Gen 2) on arm64, LDMD on a
#                 drum module, AC50 on MPC Sample and HV03 on VX5.
#   BINWALK       1 to extract with binwalk instead of QEngine's readers. See
#                 extract_rootfs.sh.
#
# Requires: python3 (binwalk with BINWALK=1), qemu-img, docker, debugfs.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR_SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIMS_DIR="$REPO_ROOT/shims"

# What differs between the generations is only what the shims are built for, which
# dtshim table they get, and the default identity. The app, its units and its shim
# needs are otherwise the same. The arm64 default is ACVA3 because Gen 2 has no MPC X
# to match Gen 1's, and the One is the simplest model in the image.
ARCH="${ARCH:-armhf}"
case "$ARCH" in
    armhf) SHIM_PLATFORM="linux/arm/v7"; SHIM_MACHINE="armv7l|armv8l|armhf"
           DT_SOC="SOC_RK3288"; LOADER="ld-linux-armhf.so.3"
           MPC_DEFAULT_CODE="ACV5"; DISK_LAYOUT="mpc" ;;
    arm64) SHIM_PLATFORM="linux/arm64"; SHIM_MACHINE="aarch64|arm64"
           DT_SOC="SOC_RK3588"; LOADER="ld-linux-aarch64.so.1"
           MPC_DEFAULT_CODE="ACVA3"; DISK_LAYOUT="engine" ;;
    *) echo "ERROR: ARCH must be armhf or arm64 (got '$ARCH')." >&2; exit 1 ;;
esac
# Left empty when not given: the default depends on which app the rootfs holds,
# which is only known once it is mounted, so the privileged container resolves it.
PRODUCT_CODE="${PRODUCT_CODE:-}"
# The builders mount the whole shims tree, so a shim shared between them is
# reached at the same path in either: /shims/<name> for the shared ones,
# /shims/rk3288 or /shims/rk3588 for the two that are genuinely per-SoC.
# Names the compiled output of the shared shims, so one source yields one
# artifact per architecture: shims/<name>/<name>_$SHIM_ARCH.
SHIM_ARCH="$ARCH"

OUT_PATH="$REPO_ROOT/build/rootfs_out.img"
SIZE=4294967296
FORCE=0
# Defaulted so that omitting --firmware reaches the check below instead of
# dying with `FIRMWARE_IMG: unbound variable` under `set -u`.
FIRMWARE_IMG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --firmware) FIRMWARE_IMG="$2"; shift 2 ;;
        --out) OUT_PATH="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

if [ ! -f "$FIRMWARE_IMG" ]; then
    echo "ERROR: Valid firmware image required: $FIRMWARE_IMG" >&2
    exit 1
fi

if [ -e "$OUT_PATH" ] && [ "$FORCE" -ne 1 ]; then
    echo "ERROR: $OUT_PATH already exists — refusing to overwrite (pass --force to replace it)." >&2
    exit 1
fi

EXTRACTOR=python3; [ "${BINWALK:-0}" = 1 ] && EXTRACTOR=binwalk
for bin in "$EXTRACTOR" qemu-img docker file; do
    command -v "$bin" >/dev/null 2>&1 || { echo "ERROR: '$bin' is required but not found on PATH." >&2; exit 1; }
done

OUT_DIR="$(cd "$(dirname "$OUT_PATH")" && pwd)"
OUT_NAME="$(basename "$OUT_PATH")"
mkdir -p "$OUT_DIR"

# Pin the host-architecture containers explicitly. Docker caches images under a
# bare tag regardless of the platform they were pulled for, so once anything has
# pulled debian:bookworm-slim for arm64 (this script's own shim container does,
# and so does the documented binfmt check), a later `docker run` with no
# --platform silently reuses the arm64 image and runs emulated. That made the
# privileged container's architecture depend on pull order rather than on intent.
case "$(uname -m)" in
    x86_64|amd64)   HOST_PLATFORM="linux/amd64" ;;
    aarch64|arm64)  HOST_PLATFORM="linux/arm64" ;;
    *)              HOST_PLATFORM="" ;;
esac

### 1. Extract the rootfs partition #########################################

# Shared with the other rootfs builder and with new_instance.sh, which needs the
# same extraction to identify a firmware's device family before it can choose
# between us. It sets EXTRACTED_ROOTFS_SIZE and cleans up its own scratch dir.
# shellcheck source=extract_rootfs.sh
. "$SCRIPT_DIR_SELF/extract_rootfs.sh"

extract_rootfs "$FIRMWARE_IMG" "$OUT_PATH"

# MPC Sample 1.3.0 and VX5 1.2.2 are arm64 like Gen 2, but their RK3566 PCB and SPI panel
# properties differ. Select the table from the extracted app, not a product code
# supplied by the caller, so identity overrides cannot pick the wrong SoC.
DEBUGFS="$(command -v debugfs || echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs)"
[ -x "$DEBUGFS" ] || { echo "ERROR: debugfs is required (e2fsprogs)." >&2; exit 1; }
if "$DEBUGFS" -R 'stat /usr/bin/launch-ac50' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:' ||
   "$DEBUGFS" -R 'stat /usr/bin/HV03' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:'; then
    [ "$ARCH" = arm64 ] || { echo "ERROR: AZ07 applications require ARCH=arm64." >&2; exit 1; }
    DT_SOC="SOC_RK3566"
fi
BUILD_VX5SURFACE=0
if "$DEBUGFS" -R 'stat /usr/bin/HV03' "$OUT_PATH" 2>/dev/null | grep -q 'Inode:'; then
    BUILD_VX5SURFACE=1
fi

### 2. Grow the image and filesystem #########################################

echo "--- resizing image to $SIZE bytes ---"
qemu-img resize -f raw "$OUT_PATH" "$SIZE"

### 2b. Build the shims ######################################################
# The shim binaries are .gitignored (*.so, plus the shared shims' per-arch
# outputs by name), so
# a fresh clone has sources only — this step is what makes the install step
# below work at all rather than silently depending on artifacts a previous
# session happened to leave in the working tree. Building them here also
# means an edited .c can never be shadowed by a stale .so.
#
# debian:bookworm for glibc 2.36, comfortably older than the guest's
# — see docs/BUILDING.md, "Toolchain". One container for all
# of them, since the apt-get dominates the cost.

echo "--- building shims from source ---"
# `docker run --platform` does not re-pull: if the tag is already cached for a
# different architecture Docker reuses that image, so the platform actually used
# depends on pull order. The comment above pins intent; this pull makes it true.
docker pull -q --platform "$SHIM_PLATFORM" debian:bookworm >/dev/null
docker run --rm --platform "$SHIM_PLATFORM" \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -e SHIM_MACHINE="$SHIM_MACHINE" \
    -e DT_SOC="$DT_SOC" \
    -e BUILD_VX5SURFACE="$BUILD_VX5SURFACE" \
    -v "$SHIMS_DIR:/shims" \
    debian:bookworm bash -c '
        set -e
        # These shims are copied straight into a $SHIM_ARCH rootfs, so a
        # wrong-architecture container here would graft foreign binaries in. Fail loudly instead.
        uname -m | grep -qxE "$SHIM_MACHINE" || {
            echo "ERROR: shim container is $(uname -m), expected $SHIM_MACHINE." >&2; exit 1; }
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq gcc libc6-dev >/dev/null 2>&1

        gcc -shared -fPIC -O2 -Wall \
            -o /shims/dtshim/dtshim_$SHIM_ARCH.so /shims/dtshim/dtshim.c -D$DT_SOC -ldl -lpthread
        gcc -O2 -Wall \
            -o /shims/touchbridge/touchbridge_$SHIM_ARCH /shims/touchbridge/touchbridge.c
        # Built for every app this script handles and installed only for MPC.
        # It needs the kernel sound headers, which libc6-dev brings, and no
        # alsa-lib: it answers below it. (No apostrophes in here, this is all
        # one single-quoted string.)
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/mpcsurface/mpcsurface_$SHIM_ARCH.so /shims/mpcsurface/mpcsurface.c -ldl -lpthread
        # For MPC and VX5, which it gives sound through a second card. It
        # resolves alsa-lib out of the app it is preloaded into.
        gcc -shared -fPIC -O2 -Wall \
            -o /shims/alsashim/alsashim_$SHIM_ARCH.so /shims/alsashim/alsashim.c -ldl -lpthread
        if [ "$BUILD_VX5SURFACE" = 1 ]; then
            apt-get install -y -qq libasound2-dev >/dev/null 2>&1
            gcc -O2 -Wall -o /shims/vx5surface/vx5surface_$SHIM_ARCH \
                /shims/vx5surface/vx5surface.c -lasound
        fi
    '

# The shared shims are checked separately: they live outside SHIMS_DIR, and each
# one is named for the architecture it was built for rather than for a device.
for artifact in touchbridge/touchbridge_$SHIM_ARCH \
                dtshim/dtshim_$SHIM_ARCH.so \
                mpcsurface/mpcsurface_$SHIM_ARCH.so \
                alsashim/alsashim_$SHIM_ARCH.so; do
    [ -s "$SHIMS_DIR/$artifact" ] || {
        echo "ERROR: shim build produced no $artifact" >&2; exit 1; }
done

### 3-5. e2fsck/resize2fs + telemetry block + shims + root password, via a
### privileged container with real loop-device support #######################

INNER_SCRIPT="$(mktemp /tmp/build-mpc-rootfs-inner.XXXXXX)"
trap 'rm -f "$INNER_SCRIPT"' EXIT

cat > "$INNER_SCRIPT" <<'DOCKER_SCRIPT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq e2fsprogs util-linux >/dev/null 2>&1
# NOTE: this container intentionally runs the *host* architecture, not the guest's —
# e2fsck/resize2fs on a multi-GB image is far slower under qemu-user emulation.
# So it must never be the source of anything that ends up inside the guest
# rootfs. Nothing here is: the shims come from the $ARCH container above.

IMG="/out/$OUT_NAME"

# The steps the rootfs builders share, so a change to one lands in all of them.
# Each file in rootfs_steps/ defines one function and explains what it is for. The
# calls below read as the sequence they are.
for _step in /steps/*.sh; do . "$_step"; done

resize_filesystem "$IMG"

echo "--- mounting via loop device ---"
LOOPDEV="$(losetup -f)"
# losetup -f asks the kernel via /dev/loop-control for the next free number, but
# the node itself only exists in this container's /dev if it already existed when
# the container started. On a host with many loops already taken (snap mounts hold
# dozens) the answer is a number above anything present, and losetup then fails
# with "No such file or directory". Create the node ourselves — we are privileged,
# loop is major 7, and the minor is the loop number.
[ -e "$LOOPDEV" ] || mknod "$LOOPDEV" b 7 "${LOOPDEV##*/loop}"
losetup "$LOOPDEV" "$IMG"
mkdir -p /mnt/rootfs
# extents/64bit are ext4 features even though `file` labels this ext2
# (no journal) — mount as ext4 so the kernel driver understands them.
mount -t ext4 "$LOOPDEV" /mnt/rootfs
cleanup() { umount /mnt/rootfs || true; losetup -d "$LOOPDEV" || true; }
trap cleanup EXIT

# Guard against firmware from the wrong device family. Otherwise this build
# completes happily and the guest panics ~45s into boot with an opaque
# "request_module: modprobe binfmt-464c" as run-init fails to exec an /sbin/init of
# the wrong architecture. The dynamic loader's filename is architecture-specific,
# so its presence is an unambiguous check.
if [ ! -e "/mnt/rootfs/lib/$LOADER" ]; then
    echo "ERROR: this firmware's rootfs is not $ARCH (no /lib/$LOADER)." >&2
    echo "       Gen 1 MPC images are armhf and Gen 2 are arm64; set ARCH to match," >&2
    echo "       or build through new_instance.sh, which reads it off the firmware." >&2
    exit 1
fi
# The app decides the unit everything below hangs off and the default identity.
# LDMD is the drum module's default because it is the code the app names its USB
# volume after; the image also carries LDMF, and both have controller firmware in
# /usr/share/Alesis/DrumModule/Resources/firmware.
if [ -f /mnt/rootfs/usr/bin/MPC ]; then
    APP_UNIT="acvs.service"; DEFAULT_CODE="$MPC_DEFAULT_CODE"
elif [ -f "/mnt/rootfs/usr/bin/MPC Sample" ]; then
    [ "$ARCH" = arm64 ] || { echo "ERROR: MPC Sample requires arm64." >&2; exit 1; }
    APP_UNIT="ac50.service"; DEFAULT_CODE="AC50"
elif [ -f /mnt/rootfs/usr/bin/HV03 ]; then
    [ "$ARCH" = arm64 ] || { echo "ERROR: VX5 requires arm64." >&2; exit 1; }
    APP_UNIT="HV03.service"; DEFAULT_CODE="HV03"
elif [ -f /mnt/rootfs/usr/bin/DrumModule ]; then
    [ "$ARCH" = armhf ] || {
        echo "ERROR: drum module images are armhf; nothing here knows an $ARCH one." >&2; exit 1; }
    APP_UNIT="alesis-drummodule.service"; DEFAULT_CODE="LDMD"
else
    echo "ERROR: no MPC, MPC Sample, VX5 or DrumModule application in this rootfs." >&2
    exit 1
fi
PRODUCT_CODE="${PRODUCT_CODE:-$DEFAULT_CODE}"
# LDMF keeps its kit content on an eMMC partition (content.mount, PARTUUID
# 4054a70b-...) that no QEngine disk provides, so its overlay on /sdcard/bfd has no
# lower layer and the app comes up without storage. Refused rather than built into
# an instance that boots and looks fine until it is used.
if [ "$APP_UNIT" = alesis-drummodule.service ] && [ "$PRODUCT_CODE" != LDMD ]; then
    echo "ERROR: only LDMD builds as a drum module; $PRODUCT_CODE needs a content" >&2
    echo "       partition QEngine does not provide yet." >&2
    exit 1
fi
echo "--- app unit $APP_UNIT, product code $PRODUCT_CODE ---"

block_telemetry /mnt/rootfs
blank_root_password /mnt/rootfs

echo "--- inserting shims into /root ---"
cp -a /shims/dtshim/dtshim_$SHIM_ARCH.so /mnt/rootfs/root/dtshim.so
chmod 755 /mnt/rootfs/root/dtshim.so

# The devicetree properties dtshim.c remaps, at the real paths for this SoC; only
# the values are ours. Every one of them must exist, because the shim remaps
# unconditionally and a missing target turns a working read into ENOENT. The
# Gen 2 dtbs in 3.10.0 declare inmusic,product-code and nothing else at the root,
# so RK3588's table, written for Engine, already covers them.
#
# PRODUCT_CODE selects which device this pretends to be.
write_fake_dt /mnt/rootfs "$PRODUCT_CODE"

# MPC Sample and VX5 use physical controls rather than touchscreens.
if [ "$APP_UNIT" != ac50.service ] && [ "$APP_UNIT" != HV03.service ]; then
    echo "--- wiring touchbridge.service ---"
    # QEMU's tablet, usb-tablet or the virtio one, presents as an absolute *mouse*.
    # MPC only responds to a real touchscreen, so the bridge re-emits that pointer as
    # a uinput multitouch device. It must start before the app's unit, which the
    # shipped touchbridge unit names for both apps: MPC enumerates input once, at its
    # own startup, and the drum module is the same code.
    cp -a /shims/touchbridge/touchbridge_$SHIM_ARCH /mnt/rootfs/root/touchbridge_mpc
    chmod 755 /mnt/rootfs/root/touchbridge_mpc
    cp -a /shims/touchbridge/touchbridge_mpc.service /mnt/rootfs/etc/systemd/system/touchbridge_mpc.service
    ln -sf ../touchbridge_mpc.service /mnt/rootfs/etc/systemd/system/multi-user.target.wants/touchbridge_mpc.service

fi

echo "--- disabling the tty1 getty (Application's display) ---"
# MPC renders fullscreen via KMS on the same VT the console getty
# lives on, and the getty keeps reading the keyboard underneath it. Every
# keystroke therefore goes to *both* the app and a root login shell you cannot
# see.
#
# Removing the enablement symlink disables it; masking getty@tty1 and
# autovt@tty1 (autovt@ is an alias of getty@, which logind spawns on VT
# allocation) stops anything bringing it back.
#
# The *serial* getty is deliberately left alone — serial-getty@ttyAMA0 is a
# different template and remains the way in on -serial stdio.
rm -f /mnt/rootfs/etc/systemd/system/getty.target.wants/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/getty@tty1.service
ln -sf /dev/null /mnt/rootfs/etc/systemd/system/autovt@tty1.service


echo "--- preloading dtshim into $APP_UNIT ---"
# A drop-in rather than an edit, so the vendor unit stays as shipped. systemd
# reads drop-ins after the main unit and last assignment wins for a given
# variable, so this would silently replace an LD_PRELOAD the vendor unit set.
# Nothing in this firmware sets one; the guard keeps that from becoming a silent
# breakage if a future firmware does. On the drum module the preload also reaches
# the plain `cat` az01-launch-DrumModule reads its product code with, which is
# what picks the controller firmware it hands az01-firmware-tool.
for _unit in /mnt/rootfs/etc/systemd/system/$APP_UNIT \
             /mnt/rootfs/lib/systemd/system/$APP_UNIT \
             /mnt/rootfs/usr/lib/systemd/system/$APP_UNIT; do
    [ -f "$_unit" ] || continue
    if grep -q 'LD_PRELOAD' "$_unit"; then
        echo "ERROR: $_unit already sets LD_PRELOAD; this drop-in would replace it." >&2
        echo "       Merge the two by hand rather than overriding blindly." >&2
        exit 1
    fi
done
# MPC and Force also get their control surface. It is a preload because MPC looks
# for its panel among the sound cards, as a USB card with Akai's ids, and opens it
# as rawmidi, where Engine's midisurface only has to be a sequencer client. It picks
# the panel from the product code written above and does nothing for a code it does
# not know. MPC Sample looks the same way for a serial card named "Control Surface",
# which mpcsurface is too, as AC50. VX5 reads its panel as input devices, which
# vx5surface supplies as a service below. The drum module reaches its panel some
# other way, not looked at yet.
PRELOAD=/root/dtshim.so
if [ "$APP_UNIT" = acvs.service ] || [ "$APP_UNIT" = ac50.service ]; then
    cp -a /shims/mpcsurface/mpcsurface_$SHIM_ARCH.so /mnt/rootfs/root/mpcsurface.so
    chmod 755 /mnt/rootfs/root/mpcsurface.so
    PRELOAD="$PRELOAD:/root/mpcsurface.so"
fi
if [ "$APP_UNIT" = acvs.service ] || [ "$APP_UNIT" = ac50.service ] ||
   [ "$APP_UNIT" = HV03.service ]; then
    # alsashim carries the app's sound out to QEMU's card, see below.
    cp -a /shims/alsashim/alsashim_$SHIM_ARCH.so /mnt/rootfs/root/alsashim.so
    chmod 755 /mnt/rootfs/root/alsashim.so
    PRELOAD="$PRELOAD:/root/alsashim.so"
fi
mkdir -p "/mnt/rootfs/etc/systemd/system/$APP_UNIT.d"
cat > "/mnt/rootfs/etc/systemd/system/$APP_UNIT.d/override.conf" <<EOF
[Service]
Environment=LD_PRELOAD=$PRELOAD
EOF

if [ "$APP_UNIT" = acvs.service ] || [ "$APP_UNIT" = ac50.service ]; then
    echo "--- giving MPC a sound card, and its output to QEMU's ---"
    # MPC opens its internal card by udev path, /dev/snd/by-path/platform-sound, as
    # VX5 does. With nothing there 3.10.0 puts "No internal audio device could be
    # found!" over the UI, and only a touch closes it. It needs a duplex card that
    # keeps time at 128-frame periods, its only size: it crashes within a second
    # on 256 or 512. QEMU's hda cannot be that card, since it moves samples every
    # 10 ms and the coreaudio backend has no capture at all, and snd-aloop moves in
    # 4 ms jiffies, on which MPC's streams stall a tick after starting. snd-dummy
    # keeps time on hrtimers, so MPC gets that, built by get_kernel.sh, and
    # alsashim copies the first output pair of everything MPC plays onto the hda
    # card, which run_qemu.sh gives the mpc family. Capture is silent.
    #
    # MPC Sample 1.3.0 opens the same path at the same period, but as four
    # channels of S32_LE at 44.1 kHz, which snd-dummy's own card does not offer.
    # get_kernel.sh adds a qengine-ac50 model that does, chosen here only for the
    # Sample, so Gen 2 keeps the card it was heard on. Its first pair is the
    # speaker and line out and follows MAIN VOLUME, which mpcsurface reports.
    DUMMY_ARGS=""
    [ "$APP_UNIT" = ac50.service ] && DUMMY_ARGS=" model=qengine-ac50"
    mkdir -p /mnt/rootfs/etc/udev/rules.d
    cat > /mnt/rootfs/etc/udev/rules.d/90-qengine-mpc-audio.rules <<'EOF'
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{id}=="Dummy", SYMLINK+="snd/by-path/platform-sound"
EOF
    cat > /mnt/rootfs/etc/systemd/system/qengine-mpc-audio.service <<EOF
[Unit]
Description=Supply MPC with a dummy card as its internal audio
After=systemd-udevd.service
Before=$APP_UNIT

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe snd-dummy$DUMMY_ARGS
ExecStart=/usr/bin/udevadm settle --timeout=10
EOF
    # alsashim does only the second output here: MPC's card keeps its own name
    # and its hw: device, and its ring and period stay as MPC asks. QEMU's card
    # is hda, id Intel, on arm64, and virtio-sound, id SoundCard, on armhf, whose
    # kernel has no hda driver (arch_audio_devices in scripts/qemu/arch_devices.sh).
    QEMU_CARD=Intel
    [ "$ARCH" = armhf ] && QEMU_CARD=SoundCard
    cat >> "/mnt/rootfs/etc/systemd/system/$APP_UNIT.d/override.conf" <<EOF
Environment=ALSASHIM_AS=off ALSASHIM_NOPLUG=1 ALSASHIM_BUFFER_SCALE=1 ALSASHIM_MIDI_CARD=-1
Environment=ALSASHIM_CUE_PCM=plughw:$QEMU_CARD ALSASHIM_CUE_CHANNEL=1

[Unit]
Wants=qengine-mpc-audio.service
After=qengine-mpc-audio.service
EOF
fi

if [ "$APP_UNIT" = HV03.service ]; then
    echo "--- giving VX5 a sound card, its output to QEMU's, and its panel ---"
    # VX5 1.2.2 crashes without onboard audio. It opens the card by the same udev
    # path as MPC, as stereo S32_LE at 48 kHz in 128-frame periods, 2.7 ms, which
    # is MPC's problem again: on snd-aloop's jiffies timer VX5 logs XRUNs both
    # ways. snd-dummy's qengine-ac50 model offers that shape too, and keeps time.
    # alsashim plays VX5's output pair on QEMU's card and, because QEMU on macOS
    # cannot record, replaces what VX5 hears with /data/input.wav, or silence
    # while there is none.
    mkdir -p /mnt/rootfs/etc/udev/rules.d
    cat > /mnt/rootfs/etc/udev/rules.d/90-qengine-vx5-audio.rules <<'EOF'
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{id}=="Dummy", SYMLINK+="snd/by-path/platform-sound"
EOF
    cat > /mnt/rootfs/etc/systemd/system/qengine-vx5-audio.service <<'EOF'
[Unit]
Description=Supply VX5 with a dummy card as its onboard audio
After=systemd-udevd.service
Before=HV03.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe snd-dummy model=qengine-ac50
ExecStart=/usr/bin/udevadm settle --timeout=10
EOF

    # The panel, phantom-power key and volume knob as uinput devices,
    # linked to the paths HV03 opens, and the f_midi port it binds by name.
    # QEMU's own gpio-keys device already has the phantom key's path, so ours
    # outranks it.
    cp -a /shims/vx5surface/vx5surface_$SHIM_ARCH /mnt/rootfs/root/vx5surface
    chmod 755 /mnt/rootfs/root/vx5surface
    cat > /mnt/rootfs/etc/udev/rules.d/90-qengine-vx5-panel.rules <<'EOF'
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="QEngine VX5 panel", SYMLINK+="input/by-path/platform-fe620000.spi-cs-0-event"
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="QEngine VX5 volume", SYMLINK+="input/by-path/platform-adc-joystick-event"
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="QEngine VX5 phantom power", SYMLINK+="input/by-path/platform-gpio-keys-event", OPTIONS+="link_priority=10"
EOF
    cat > /mnt/rootfs/etc/systemd/system/vx5surface.service <<'EOF'
[Unit]
Description=Virtual VX5 panel and USB MIDI port
After=systemd-udevd.service
Before=HV03.service

[Service]
Type=simple
# Driven by hand with: echo 'press autotune' > /run/vx5surface.fifo
# Held open read-write so a writer closing it is not the end of input.
ExecStartPre=/bin/sh -c 'rm -f /run/vx5surface.fifo && mkfifo /run/vx5surface.fifo'
ExecStart=/bin/sh -c 'exec 3<>/run/vx5surface.fifo; exec /root/vx5surface < /run/vx5surface.fifo'
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
    ln -sf ../vx5surface.service \
        /mnt/rootfs/etc/systemd/system/multi-user.target.wants/vx5surface.service

    cat >> /mnt/rootfs/etc/systemd/system/HV03.service.d/override.conf <<'EOF'
Environment=ALSASHIM_AS=off ALSASHIM_NOPLUG=1 ALSASHIM_BUFFER_SCALE=1 ALSASHIM_MIDI_CARD=-1
Environment=ALSASHIM_CUE_PCM=plughw:Intel ALSASHIM_CUE_CHANNEL=1
Environment=ALSASHIM_INPUT_WAV=/data/input.wav

[Unit]
Requires=qengine-vx5-audio.service
After=qengine-vx5-audio.service
Wants=vx5surface.service
After=vx5surface.service

[Service]
# VX5 1.2.2's DSP worker can start with a garbage sample count. Zero fresh
# allocations, including small blocks normally returned
# from tcache, so it starts with no samples rather than a garbage-sized buffer.
# MALLOC_PERTURB_ alone leaves tcache allocations uninitialized.
Environment=MALLOC_PERTURB_=255
Environment=GLIBC_TUNABLES=glibc.malloc.tcache_count=0
EOF
fi

if [ "$APP_UNIT" = alesis-drummodule.service ]; then
    echo "--- wiring the drum module's internal SD card ---"
    # The drum module keeps its kit library and everything the user writes on an
    # internal SD card, /sdcard, with a writable overlay on /sdcard/bfd. The update
    # image carries only the rootfs, so new_instance.sh supplies the card as a blank
    # disk (make_disk.sh --family internal-sd) and the shipped
    # /usr/share/Alesis/ldmd-content-update is what fills the overlay's lower layer.
    # Without the card, 1.5.2.3 stops at "File system problem" and every write under
    # /root fails, since the rootfs is mounted read-only.
    #
    # Two vendor generators pick the storage layout from the product code, reading it
    # with a plain cat before any unit runs, so the preload on the app's unit never
    # reaches them. They see no code, take the LDMF branch, and bind the empty
    # /content over /sdcard while the app runs as LDMD. A same-named generator in
    # /etc overrides the one in /usr/lib, so each is wrapped to run the vendor
    # script, unchanged, with dtshim preloaded.
    mkdir -p /mnt/rootfs/etc/systemd/system-generators
    for _gen in ldmx-content-mount-generator ldmx-content-overlay-generator; do
        [ -x "/mnt/rootfs/usr/lib/systemd/system-generators/$_gen" ] || {
            echo "ERROR: no vendor $_gen in this rootfs to wrap." >&2; exit 1; }
        printf '#!/bin/sh\nLD_PRELOAD=/root/dtshim.so exec /usr/lib/systemd/system-generators/%s "$@"\n' \
            "$_gen" > "/mnt/rootfs/etc/systemd/system-generators/$_gen"
        chmod 755 "/mnt/rootfs/etc/systemd/system-generators/$_gen"
    done

    # The SD card's units are gated on chosen/inmusic,internal-sd-fitted, which
    # systemd tests itself, again without dtshim. An empty ConditionPathExists=
    # clears the list. media-az01-internal-sd.mount ships its own such drop-in.
    _sd_dev='dev-disk-by\x2dpath-platform\x2dff0c0000.dwmmc\x2dpart1'
    for _unit in sdcard.mount sdcard-bfd.mount "$_sd_dev.device" \
                 "systemd-fsck@$_sd_dev.service"; do
        mkdir -p "/mnt/rootfs/etc/systemd/system/$_unit.d"
        printf '[Unit]\nConditionPathExists=\n' > "/mnt/rootfs/etc/systemd/system/$_unit.d/qengine.conf"
    done

    # sdcard.mount and the app's fsck check both name the card by its RK3288 path,
    # ff0c0000.dwmmc. udev gives the QEngine disk that name, found by the filesystem
    # UUID make_disk.sh formats it with. The two must match.
    cat > /mnt/rootfs/etc/udev/rules.d/90-qengine-internal-sd.rules <<'EOF'
SUBSYSTEM=="block", ENV{ID_FS_UUID}=="5d2a1e5c-0a1b-4c2d-9e3f-51d0c0a1e5d0", SYMLINK+="disk/by-path/platform-ff0c0000.dwmmc-part1"
EOF
fi

umount /mnt/rootfs
losetup -d "$LOOPDEV"
trap - EXIT

verify_rootfs "$IMG"
DOCKER_SCRIPT

echo "--- running e2fsck/resize2fs/shim-install in a privileged container ---"
docker pull -q ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} debian:bookworm-slim >/dev/null
docker run --rm --privileged \
    ${HOST_PLATFORM:+--platform "$HOST_PLATFORM"} \
    -e OUT_NAME="$OUT_NAME" \
    -e SHIM_ARCH="$SHIM_ARCH" \
    -e ARCH="$ARCH" \
    -e LOADER="$LOADER" \
    -e MPC_DEFAULT_CODE="$MPC_DEFAULT_CODE" \
    -e PRODUCT_CODE="$PRODUCT_CODE" \
    -v "$OUT_DIR:/out" \
    -v "$SHIMS_DIR:/shims:ro" \
    -v "$SCRIPT_DIR_SELF/rootfs_steps:/steps:ro" \
    -v "$INNER_SCRIPT:/inner.sh:ro" \
    debian:bookworm-slim bash /inner.sh

if [ ! -s "$OUT_PATH" ]; then
    echo "FAILED: expected output file is missing from $OUT_PATH." >&2
    exit 1
fi

echo ""
echo "Built: $OUT_PATH"
echo ""
file "$OUT_PATH"
echo ""
echo "Still needed to boot: kernel+initrd (get_kernel.sh --arch $ARCH) and an"
echo "/data disk (make_disk.sh --family $DISK_LAYOUT) — see BUILD_MPC.md."