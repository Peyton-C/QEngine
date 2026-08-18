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
- It does still share the device-agnostic build steps in
  [rootfs_steps/](../../scripts/build_scripts/rootfs_steps/): growing the
  filesystem, blocking telemetry, blanking the root password, and the final
  consistency check. It reports crashes to the same Sentry organisation Engine
  does — `/usr/bin/MPC` carries a DSN for `o230257.ingest.sentry.io`, differing
  only in project id — so the shared host list applies here as-is. See
  [../../docs/BLOCKING_TELEMETRY.md](../../docs/BLOCKING_TELEMETRY.md).

## How MPC identifies itself, and what an emulated one is missing

MPC gets its identity from two devicetree properties, produced very differently:

- `inmusic,product-code` is **static, compiled into the dtb**, declared literally in
  the DTS next to `model`. All 14 dtbs in `/boot` of a 3.9.1 rootfs carry one:
  `ACV5` and `ACV8` (generic `InMusic MPC …` model strings), `ACV5S` (MPC X SE),
  `ACVA` (MPC One), `ACVA2` (MPC One+), `ACVB` (MPC Live Mk 2), `ACVM` (MPC Key 61),
  `ACVR` (MPC Key 37). Most also ship a `-c` variant for the `rockchip,rk3288-c` SoC
  revision, and `ACVM` appears on both `az01` and `az05`.
- `serial-number` is **not in any dtb** — zero of the 14, and zero of the DTS
  sources. U-Boot creates that node at boot, the standard Rockchip pattern, from OTP
  or eFuse or a stored env value. Nothing in the rootfs writes it, and the update
  image contains only a rootfs partition — no bootloader — so the producer is not in
  the image at all.

Neither is read by direct `open()` alone: MPC goes through `libaz0x-info.so.0.9`
(`az0x_info_get`, `az0x_info_datum_at`/`_key`/`_value`/`_category`, …), which exposes
both as keyed data alongside a great deal more — `board`, `bootloader`, `platform`,
`secure-boot`, `cpuid` from the RK3288 eFuse, the eMMC and SD CID sets,
`touch-panel`, `touch-firmware`, `wireless-chip`, `usb-hub`, `gpu-driver`, `typec`,
`panel`. MPC also contains the raw sysfs path strings itself, so some reads may
bypass the library; which call sites do what is not traced.

**Three different things are called a serial here** and should not be conflated:
`serial` is the unit serial from the devicetree, `emmc-serial` comes from the eMMC
CID, and `sd-serial` from the SD card. Separately, MPC has a serial *write* path
aimed at the control-surface MCU rather than the devicetree
(`control_surface.serialnumber_reprogram`, a `SerialNumber` listener on
`AcvxHardwareIO`, and a `ProgramSerialNumber unimplemented!` string suggesting at
least one branch is unfinished in this build) — the same family as `midifirmup`'s
`WriteSerialNumber` on the Denon side.

`/usr/bin/az0x-info` is a CLI over the same library that dumps everything as
`key="value"` pairs, so on a booted guest it is the one command that shows exactly
what the app sees, including whether `serial` came back empty. `az01-info` is a
symlink to it.

**What an emulated MPC gets.** A guest boots on QEMU's own devicetree, which has
neither property, so the build installs `dtshim` — the same RK3288 shim the armv7
Engine build uses, unchanged, because these dtbs declare nothing it does not already
serve. It is preloaded into `acvs.service` through a drop-in, so the vendor unit
stays as shipped, and the build fails rather than silently replacing an `LD_PRELOAD`
if a future firmware sets one itself. `write_fake_dt` writes the identity:

```sh
PRODUCT_CODE=ACVA2 ./build_mpc_rootfs.sh --firmware <img> --force   # MPC One+
```

Default `ACV5`. Every build before this one shipped without it, and MPC ran fine
identifying as nothing — so if a guest misbehaves in a way that correlates with the
code, that is new territory and the code is the first thing to vary.

To see what the app sees, on the guest:

```sh
LD_PRELOAD=/root/dtshim.so az0x-info
```

The preload is needed because a login shell has none — `acvs.service` gets it from
the drop-in, an interactive command does not. Without it `serial` and
`product-code` come back empty, which is also a quick way to confirm the shim is
what supplies them.

`serial-number` is left at `write_fake_dt`'s default. Nothing here varies by it and
no shipped devicetree has one to match, but it is the second argument if a
per-instance serial is ever wanted. See
[../../shims/dtshim/README.md](../../shims/dtshim/README.md).
