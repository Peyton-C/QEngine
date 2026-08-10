# Setup everything required to emulate arm64 Engine OS
Requires docker, qemu, binwalk 3.1.X.

1. [Download Engine from InMusic](https://enginedj.com/downloads) for the RANE SYSTEM ONE.
2. Build the rootfs with `build_arm64_rootfs.sh --firmware PATH_TO_SYSTEMONE_UPDATE.img`.
3. Get the appropriate kernel and initrb with `get_arm64_kernel.sh`.
4. Make a data disk for Engine with `make_data_disk.sh`.
5. Boot Engine with the correct script for your system in `scripts/qemu/`.