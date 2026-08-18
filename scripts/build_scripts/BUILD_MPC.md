# Setup everything required to emulate an armv7 Akai MPC
Requires docker, qemu, binwalk 3.1.X, e2fsprogs.

1. [Download MPC firmware from Akai](https://www.akaipro.com/downloads-and-support/downloads/firmware/mpc/) — the **Gen 1** `.img` covers the RK3288 models (MPC X, X SE, Live, Live 2, One, One+, Key 61, Key 37).
2. Build the rootfs with `build_mpc_rootfs.sh --firmware PATH_TO_MPC_UPDATE.img`.
3. Get the kernel and initrd with `get_kernel.sh --arch armhf`.
4. Make a /data disk for MPC with `make_disk.sh --family mpc`.
5. Boot with `DEVICE=mpc ARCH=armhf scripts/qemu/run_qemu.sh`.

Or let one command do all of it, with the device family and architecture identified from
the firmware — see [INSTANCES.md](INSTANCES.md):

```sh
scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --firmware PATH_TO_MPC_UPDATE.img
scripts/qemu/run_instance.sh --name mpc-3.9.1
```

Touch works out of the box: the rootfs build installs `touchbridge_mpc` and starts it
before `acvs.service`. MPC only responds to a real touchscreen, and QEMU's virtio
tablet presents as an absolute mouse, so the bridge re-emits it as a uinput
multitouch device.

To build and run several devices or firmware versions side by side, see
[INSTANCES.md](INSTANCES.md).

## Notes

- Used directly as above, the rootfs is written to `build/rootfs_out.img` — the
  same path the arm64 build uses, so building one target overwrites the other.
  Use an instance (see INSTANCES.md) to keep several targets side by side.
- Only the **signed** firmware images extract with `binwalk` alone. Unsigned ones
  (Prime 4, most HeadRush) come out as a single bogus "DTB" and need `mpcimg`
  first — see [../../docs/BUILDING.md](../../docs/BUILDING.md).
- MPC drives KMS directly and links no Mali or EGL, which is why it needs none of
  the shim stack Engine does. `readelf -d <app-binary> | grep -iE 'mali|EGL'`
  coming back empty is a good predictor that a device will work here.
- MPC needs none of the shim stack Engine does, but it does share the
  device-agnostic build steps in
  [rootfs_steps/](../../scripts/build_scripts/rootfs_steps/): growing the
  filesystem, blocking telemetry, blanking the root password, and the final
  consistency check. It reports crashes to the same Sentry organisation Engine
  does — `/usr/bin/MPC` carries a DSN for `o230257.ingest.sentry.io`, differing
  only in project id — so the shared host list applies here as-is. See
  [../../docs/BLOCKING_TELEMETRY.md](../../docs/BLOCKING_TELEMETRY.md).
