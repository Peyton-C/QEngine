#!/bin/bash
# sshd.sh : start sshd in the booted guest from its serial console, so the other
# scripts can reach it with guest().
#
# Engine OS ships sshd disabled and root without a password login over ssh, so
# the key goes to /run, as docs/REMOTE_ACCESS.md describes, and nothing is
# written into the rootfs.
set -eu
source "$(dirname "$0")/common.sh"
pub=$(cat "$KEY.pub")
python3 "$TEST_SCRIPTS/serial.py" "$SERIAL_PORT" true 5 > /dev/null || true
python3 "$TEST_SCRIPTS/serial.py" "$SERIAL_PORT" "mkdir -p /run/sshd; echo '$pub' > /run/qengine_authorized_keys; systemctl stop sshd 2>/dev/null; /usr/sbin/sshd -o AuthorizedKeysFile=/run/qengine_authorized_keys && echo sshd started" 20
