#!/bin/bash
# Builds the Debian trixie armhf kernel and initrd. A wrapper: the work is in
# get_kernel.sh, which the two architectures now share instead of being two copies
# differing in six values. Kept as a file of its own because BUILD_ARM64.md and
# BUILD_MPC.md tell readers to run it by this name.
exec "$(dirname "${BASH_SOURCE[0]}")/get_kernel.sh" --arch armhf "$@"
