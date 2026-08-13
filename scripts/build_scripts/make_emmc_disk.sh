#!/bin/bash
# Builds the writable disk for the mpc family. A wrapper: the work is in make_disk.sh,
# where the partition table is a two-line array rather than two near-identical scripts.
# Kept as a file of its own because BUILD_ARM64.md and BUILD_MPC.md tell readers to run
# it by this name, and new_instance.sh's family registry records it.
exec "$(dirname "${BASH_SOURCE[0]}")/make_disk.sh" --family mpc "$@"
