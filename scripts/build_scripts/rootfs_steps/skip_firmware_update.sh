# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Makes Engine skip the control-surface firmware check by adding
# -skipFirmwareUpdate to its invocation in /usr/Engine/Scripts/engine.
#
#   skip_firmware_update [rootfs mount point]    (default /mnt/rootfs)
#
# Guest-facing and architecture-independent: it edits one shell script inside the
# mounted rootfs and knows nothing about shims, device codes or the CPU. That is
# what makes it shareable, where most of the builders' other steps are not --
# they differ by architecture, shim name or service name. Kept beside
# extract_rootfs.sh, and for the same reason: both builders needed it, so a copy
# in each would be a copy to keep in sync.
#
# Engine compares the revision bytes the control surface reports in its device
# inquiry against /usr/Engine/Firmware/<CODE> Controller/firmware.json on every
# boot, and on any difference quits with UpdateFirmware and hands off to
# /usr/Engine/FirmwareUpdater. Under emulation that hand-off can never finish:
# the surface is midisurface, a userspace MIDI client with no flashable MCU
# behind it, so the updater times out and engine.service restarts straight back
# into the same check. The guest never reaches the UI.
#
# Answering the check honestly was tried and abandoned -- midisurface used to
# read firmware.json and echo the version back, and no longer does. It is not
# one version but a tree of them: KnownDevices.vfsb declares sub-boards per
# product (JP08 and JP14 carry AdditionalDevices "<CODE> Motor,37,784"), and
# Engine reads each board's version from a different *offset into the same
# inquiry reply* -- byte 37 for the motor where the controller is byte 11 -- in
# one of two encodings chosen per product. Every board that still mismatched
# would then need the flash protocol answered as well, ack by ack. The flag is
# one word and covers all of it.
#
# It is a real QCommandLineOption, not a debug leftover: Engine tests it at the
# top of its per-device "does this need an update?" function and returns "no"
# before firmware.json is read, so the version path never reaches
# quitAndStartFirmwareUpdate(). Confirmed by reading Engine 2.4.0, 3.4.0, 4.3.4
# and 5.0.4; absent from 1.x, which these builders do not target.
#
# Not covered: the separate DFU-mode path, which fires only when dfu-util -l
# lists a device. Nothing we present is a USB DFU device.
skip_firmware_update() {
    _rootfs="${1:-/mnt/rootfs}"
    _launcher="$_rootfs/usr/Engine/Scripts/engine"

    echo "--- making Engine skip control-surface firmware updates ---"
    [ -f "$_launcher" ] || {
        echo "ERROR: no /usr/Engine/Scripts/engine in this rootfs." >&2; return 1; }
    [ -f "$_launcher.stock" ] || cp -a "$_launcher" "$_launcher.stock"

    # Idempotent: these builders get re-run against an already-built rootfs. The
    # guard is on the sed itself as well as around it, because the substitution
    # alone would happily insert the flag a second time.
    if grep -q -- '-skipFirmwareUpdate' "$_launcher"; then
        echo "    already present"
        return 0
    fi
    sed -i '/-skipFirmwareUpdate/!s|^\([[:space:]]*/usr/Engine/Engine\) |\1 -skipFirmwareUpdate |' "$_launcher"
    grep -q -- '-skipFirmwareUpdate' "$_launcher" || {
        echo "ERROR: could not insert -skipFirmwareUpdate into $_launcher;" >&2
        echo "       its Engine invocation is not the single line expected." >&2
        return 1; }
}
