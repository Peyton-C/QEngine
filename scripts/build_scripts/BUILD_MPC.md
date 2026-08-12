# Setup everything required to emulate an armv7 Akai MPC
Requires docker, qemu, binwalk 3.1.X, e2fsprogs.

1. [Download MPC firmware from Akai](https://www.akaipro.com/downloads-and-support/downloads/firmware/mpc/) — the **Gen 1** `.img` covers the RK3288 models (MPC X, X SE, Live, Live 2, One, One+, Key 61, Key 37).
2. Build the rootfs with `build_mpc_rootfs.sh --firmware PATH_TO_MPC_UPDATE.img`.
3. Get the appropriate kernel and initrd with `get_armv7_kernel.sh`.
4. Make a /data disk for MPC with `make_emmc_disk.sh`.
5. Boot MPC with `scripts/qemu/mpc_linux.sh`.

Touch works out of the box: the rootfs build installs `touchbridge_mpc` and starts it
before `acvs.service`. MPC only responds to a real touchscreen, and QEMU's virtio
tablet presents as an absolute mouse, so the bridge re-emits it as a uinput
multitouch device.

## Notes

- The rootfs is written to `build/rootfs_out.img`, the same path the arm64 build
  uses, so building one target overwrites the other. Rebuild when switching.
- Only the **signed** firmware images extract with `binwalk` alone. Unsigned ones
  (Prime 4, most HeadRush) come out as a single bogus "DTB" and need `mpcimg`
  first — see [../../docs/BUILDING.md](../../docs/BUILDING.md).
- MPC drives KMS directly and links no Mali or EGL, which is why it needs none of
  the shim stack Engine does. `readelf -d <app-binary> | grep -iE 'mali|EGL'`
  coming back empty is a good predictor that a device will work here.
