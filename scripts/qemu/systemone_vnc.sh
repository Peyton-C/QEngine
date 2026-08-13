#!/bin/bash
# Boot arm64 Engine OS served over VNC.
#
# A wrapper: everything this used to spell out by hand now lives in run_qemu.sh, which
# resolves the machine type, devices, kernel and accelerator from FAMILY, DISPLAY_MODE
# and ARCH. The seven wrappers were near-identical copies of one command line, and the
# copies had already drifted -- this one's siblings disagreed about which audio backend
# a VNC session uses.
#
# Kept as a file of its own because BUILD_ARM64.md, BUILD_MPC.md and INSTANCES.md name
# it, and instance.env records a launcher by filename. Every override run_qemu.sh
# accepts works here too.
export FAMILY="engine"
export DISPLAY_MODE="vnc"
exec "$(dirname "${BASH_SOURCE[0]}")/run_qemu.sh" "$@"
