#!/bin/sh
# controllermap.sh — pick an Engine control-surface mapping based on which USB
# controller is actually plugged in.
#
# WHY THIS EXISTS
#
# Engine only binds a control surface that answers its MIDI device-inquiry
# handshake with inMusic's manufacturer id and this product's id (0x27) — see
# docs/ENGINEOS.md. A third-party controller cannot answer that, so it can
# never be bound directly, and this rootfs's KnownDevices table has exactly one
# entry that maps to Engine's decks. That leaves one usable arrangement:
#
#   real controller --> midisurface_rmz2 --forward --> Engine
#                       (answers the handshake, relays MIDI unchanged)
#
# with the *mapping* — which note/CC means what — supplied by the assignment
# QML that Engine loads for the RMZ2 controller. Engine reads that file from
# disk at device-bind time, so swapping it swaps the mapping, with no runtime
# translation and no per-controller code.
#
# This script does the swapping: it looks at the connected USB devices, finds
# one listed in the manifest, and installs that controller's assignments file
# over the one Engine will load.
#
# LIMITS worth knowing before relying on it
#   - One mapping is active at a time. Engine binds a single identity, so the
#     installed file simply *is* the mapping; there is no per-device selection.
#   - Only the vocabulary Engine's own QML components expose can be mapped
#     (PlayCue, Sync, MixerChannelCore, ActionPads, ...). A control with no
#     counterpart has nowhere to go.
#   - LED/display feedback is emitted in RMZ2's protocol and a foreign
#     controller will not understand it. Harmless, but expect dark buttons.
#
# Usage: controllermap.sh [--dry-run] [--list] [--restore]

set -eu

MAP_DIR="${MAP_DIR:-/root/controllermap}"
MANIFEST="$MAP_DIR/manifest"
TARGET_DIR="/usr/Engine/AssignmentFiles/PresetAssignmentFiles/RMZ2"

# Engine resolves these names from its KnownDevices entry (AssignmentFileName
# = "RMZ2 Controller"), so they are fixed — a mapping directory supplies files
# under exactly these names and they are installed over the vendor ones.
#
#   <mapping>/RMZ2_Controller_Assignments.qml   required — the note/CC map
#   <mapping>/RMZ2_Controller_Device.qml        optional — device protocol:
#       SysEx identity, LED/pad-display encoding, and the motor commands sent
#       on startup/shutdown. Most mappings will not need this; supply it only
#       to change how Engine talks *to* the surface, as opposed to what the
#       surface's controls mean.
MAPPED_FILES="RMZ2_Controller_Assignments.qml RMZ2_Controller_Device.qml"

DRY_RUN=0
ACTION=install
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --list)    ACTION=list ;;
        --restore) ACTION=restore ;;
        *) echo "unrecognized argument: $arg" >&2; exit 1 ;;
    esac
done

log() { echo "controllermap: $*"; }

# / is mounted read-only on this rootfs, so writing the assignment file needs a
# temporary remount.
#
# Restoring read-only is best-effort and *usually fails* on a running system:
# journald and friends hold files under /var open for writing, so the remount
# comes back "mount point is busy" (confirmed directly). That is reported
# rather than swallowed, because it means the rootfs is left writable — a real
# deviation from how this image normally runs, and not something to discover
# later by accident. Writes here are small and infrequent (one file, at boot),
# so the exposure is limited, but it is worth knowing about.
remount_rw() { mount -o remount,rw / 2>/dev/null || true; }
remount_ro() {
    if ! mount -o remount,ro / 2>/dev/null; then
        log "note: could not restore / to read-only (mount busy); it stays rw"
    fi
}

# Every connected USB device as lowercase "vid:pid", one per line. Read from
# sysfs rather than lsusb, which this rootfs does not ship.
connected_ids() {
    for dev in /sys/bus/usb/devices/*; do
        [ -r "$dev/idVendor" ] && [ -r "$dev/idProduct" ] || continue
        vid=$(cat "$dev/idVendor")
        pid=$(cat "$dev/idProduct")
        echo "$(echo "$vid$pid" | tr 'A-Z' 'a-z' | sed 's/\(....\)/\1:/')"
    done
}

if [ "$ACTION" = list ]; then
    log "connected USB devices:"
    connected_ids | sort -u | sed 's/^/  /'
    if [ -f "$MANIFEST" ]; then
        log "known mappings:"
        grep -v '^[[:space:]]*#' "$MANIFEST" | grep -v '^[[:space:]]*$' |
            while read -r id name _; do echo "  $id -> $name"; done
    fi
    exit 0
fi

for f in $MAPPED_FILES; do
    [ -f "$TARGET_DIR/$f" ] || { log "ERROR: $TARGET_DIR/$f not found"; exit 1; }
done

# Snapshot the vendor files once, before anything overwrites them, so every
# run installs from a known base rather than layering onto whatever ran last.
NEED_SNAPSHOT=0
for f in $MAPPED_FILES; do
    [ -f "$TARGET_DIR/$f.vendor" ] || NEED_SNAPSHOT=1
done
if [ "$NEED_SNAPSHOT" = 1 ]; then
    remount_rw
    for f in $MAPPED_FILES; do
        [ -f "$TARGET_DIR/$f.vendor" ] || cp -a "$TARGET_DIR/$f" "$TARGET_DIR/$f.vendor"
    done
    remount_ro
    log "saved vendor copies alongside the originals (*.vendor)"
fi

restore_vendor() {
    remount_rw
    for f in $MAPPED_FILES; do
        cp -a "$TARGET_DIR/$f.vendor" "$TARGET_DIR/$f"
    done
    remount_ro
}

if [ "$ACTION" = restore ]; then
    restore_vendor
    log "restored the vendor RMZ2 mapping"
    exit 0
fi

[ -f "$MANIFEST" ] || { log "no manifest at $MANIFEST — nothing to do"; exit 0; }

# Manifest lines: <vid:pid> <mapping-dir> [description...]
MATCH_ID=""
MATCH_NAME=""
for id in $(connected_ids | sort -u); do
    line=$(grep -i "^[[:space:]]*$id[[:space:]]" "$MANIFEST" 2>/dev/null | head -n 1 || true)
    if [ -n "$line" ]; then
        MATCH_ID="$id"
        MATCH_NAME=$(echo "$line" | awk '{print $2}')
        break
    fi
done

if [ -z "$MATCH_NAME" ]; then
    log "no connected USB device matches the manifest; leaving the vendor mapping in place"
    # Restore rather than leaving a previous controller's mapping installed —
    # a mapping for a device that is no longer attached is worse than none.
    for f in $MAPPED_FILES; do
        if ! cmp -s "$TARGET_DIR/$f.vendor" "$TARGET_DIR/$f"; then
            restore_vendor
            log "reverted to the vendor mapping"
            break
        fi
    done
    exit 0
fi

SRC_DIR="$MAP_DIR/mappings/$MATCH_NAME"
if [ ! -f "$SRC_DIR/RMZ2_Controller_Assignments.qml" ]; then
    log "ERROR: manifest matched $MATCH_ID -> $MATCH_NAME but"
    log "       $SRC_DIR/RMZ2_Controller_Assignments.qml is missing"
    exit 1
fi

if [ "$DRY_RUN" = 1 ]; then
    # `if` rather than `[ ... ] && log`: under `set -e` a false test as the
    # loop's last statement makes the loop — and the script — exit non-zero.
    for f in $MAPPED_FILES; do
        if [ -f "$SRC_DIR/$f" ]; then
            log "would install $MATCH_NAME/$f (matched $MATCH_ID)"
        fi
    done
    exit 0
fi

# Start from the vendor files so anything this mapping does not override is
# the stock version, not a leftover from a previously installed mapping.
restore_vendor
remount_rw
for f in $MAPPED_FILES; do
    if [ -f "$SRC_DIR/$f" ]; then
        cp -a "$SRC_DIR/$f" "$TARGET_DIR/$f"
        log "installed $f from mapping '$MATCH_NAME'"
    fi
done
remount_ro
log "mapping '$MATCH_NAME' active for $MATCH_ID"
log "Engine loads assignments when it binds the surface — (re)start"
log "engine.service after this for the change to take effect."