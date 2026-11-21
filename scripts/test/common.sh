# shellcheck shell=bash
# Sourced by the test scripts. Paths come from where the checkout is, so the
# same scripts run on the Mac and on a test host that holds a copy of scripts/.
TEST_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TEST_SCRIPTS/../.." && pwd)"
# Captures, logs, screenshots and tone images. Under build/, so never tracked.
OUT="${QENGINE_TEST_DIR:-$ROOT/build/test}"
SERIAL_PORT="${SERIAL_PORT:-4575}"
MONITOR_PORT="${MONITOR_PORT:-4576}"
mkdir -p "$OUT"
# The key guest() logs in with, made on first use. sshd.sh puts its .pub in the
# guest under /run, and deploy.sh copies it to the test host with the images.
KEY="${QENGINE_TEST_KEY:-$OUT/guest_key}"
[ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N "" -C qengine-test -f "$KEY"

# guest PORT command... : run a command in the guest over its forwarded ssh port.
guest() {
    local p=$1; shift
    ssh -q -p "$p" -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@localhost "$@"
}

# instance_var NAME VAR : read one setting from an instance's instance.env.
instance_var() {
    grep "^$2=" "$ROOT/build/instances/$1/instance.env" | cut -d= -f2-
}

# vnc HOST::PORT args... : vncdo, with --nocursor because Qt's VNC server
# crashes Legacy Engine when a client that took the rich cursor disconnects.
vnc() {
    local srv=$1; shift
    uv run -q --with vncdotool vncdo --nocursor -s "$srv" "$@"
}
