# QEngine - Engine OS Emulator
Emulate Engine OS and other InMusic OSes inside QEMU

**NOTE:** Docs are currently **awful**, they will be better eventually. Basic docs for setting up arn64 emulation are [here.](scripts/build_scripts/BUILD_ARM64.md)

## Documentation
- [BUILDING.md](docs/BUILDING.md) — building/assembling and booting an Engine OS image under QEMU
- [ENGINEOS.md](docs/ENGINEOS.md) — Engine OS internals, product spoofing, known limitations
- [BLOCKING_TELEMETRY.md](docs/BLOCKING_TELEMETRY.md) — stopping Engine's crash/analytics reporting from reaching InMusic's real Sentry project
- [BUILD_ARM64.md](scripts/build_scripts/BUILD_ARM64.md) / [BUILD_ARMV7_ENGINE.md](scripts/build_scripts/BUILD_ARMV7_ENGINE.md) / [BUILD_MPC.md](scripts/build_scripts/BUILD_MPC.md) — what each target needs
- [INSTANCES.md](scripts/build_scripts/INSTANCES.md) — running several emulated devices side by side

## Quick setup

Verified from an empty `build/` on x86_64 Linux. The same commands work on an arm64
Linux host or an Apple Silicon Mac, where the launchers pick up KVM/HVF instead of
falling back to TCG.

**Prerequisites:** `docker`, `qemu-system-aarch64` and `qemu-system-arm`, `binwalk`
3.1.x, `e2fsprogs` (for `dumpe2fs`, `debugfs`, `resize2fs`), `file`.

```sh
# 1. One instance per device + firmware version. Point --firmware straight at an
#    update image; there is no firmware directory to populate. The matching kernel
#    is detected from the rootfs and built on demand -- once per architecture,
#    then reused (~7 min arm64, ~2 min armv7, ~1 min per rootfs).
scripts/build_scripts/new_instance.sh --name rmz2-4.6.0 --device engine \
    --firmware /path/to/SYSTEMONE-4.6.0-Update.img

scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --device mpc \
    --firmware /path/to/MPC-3.9.1-Gen1-update.img

# Engine OS also ships on armv7 (the RK3288 Prime/SC/Mixstream controllers). It is
# the same --device engine, on a different architecture; which builder and disk
# layout it needs is read off the firmware rather than asked for.
scripts/build_scripts/new_instance.sh --name jp07-5.0.4 --device engine \
    --firmware /path/to/JP07-JP08-JP11-5.0.4.img

# 2. Boot. Each instance owns its disks and its host ports, so these run at the
#    same time without interfering.
scripts/qemu/run_instance.sh --name rmz2-4.6.0
scripts/qemu/run_instance.sh --name mpc-3.9.1
scripts/qemu/run_instance.sh --list
```

The serial console appears in the terminal you launched from and root has no
password. On x86_64 the guest is emulated rather than virtualized — KVM cannot
accelerate a foreign architecture — so allow roughly two minutes to a login prompt.

**Picking the right firmware.** `--device mpc` means the armv7 / RK3288 MPC family
(product codes `ACV*`). Akai also ships an arm64 "Gen 2" MPC image; that one belongs
to a different pipeline, and the build now rejects it up front rather than producing
an image that panics partway into boot.

**Build one architecture at a time.** Both pipelines use the same `debian:bookworm`
and `debian:trixie` tags at different architectures, and Docker caches a tag at only
one architecture at a time, so building an arm64 and an armv7 target concurrently
will fight over them. Sequentially is fine — each build pulls what it needs.

In the MPC guest, failing units for LoadPin verity trustpoints, AZ0x system info
logging and XMOS USB audio firmware are expected: none of that hardware exists here.

## Engine OS Support Matrix
|                         | 5.0.4 (arm64) | 5.0.4 (armv7) | 4.6.0 | 4.3.0 |
|-------------------------|---------------|---------------|-------|-------|
| Rootfs Extraction       | Y             | Y             | Y     | Y     |
| Engine                  | Y             | Y             | Y     | Y     |
| SoundSwitch             | Y             | ?             | Y     | N     |
| Native Display          | Y             | Y             | Y     | N     |
| QT VNC Display          | N             | N             | N     | Y     |
| Fake Touch              | Y             | Y             | Y     | Y     |
| Keyboard Navigation     | Y             | ?             | Y     | Y     |
| Audio Playback          | Y             | N             | Y     | N     |
| MIDI                    | Y             | ?             | Y     | ~     |
| External Media (USB/SD) | Y             | ?             | Y     | N     |

For 5.0.4 (armv7): Engine boots to a rendered, animating UI on `eglfs`/KMS, in
software (`kms_swrast` — virgl needs PCI, which the 32-bit `virt` machine lacks).
`QT VNC Display` is N for the same reason as arm64: Qt 6.7.2 ships no `libqvnc.so`.
Audio is N because the shim stack it needs was left out, not because it was tried
and failed. Touch is confirmed end-to-end against the SDL display —
`touchbridge` re-emits QEMU's absolute tablet as a uinput multitouch device and
Engine responds to it. The remaining `?` rows are untried, not known-broken. See
[BUILD_ARMV7_ENGINE.md](scripts/build_scripts/BUILD_ARMV7_ENGINE.md).

## Emulated Controllers

The **Code** column is the `inmusic,product-code` devicetree value, which is what
actually selects the device — one firmware image usually serves several products and
`/usr/Engine` is shared across them, so the code is the only thing distinguishing
them (see [ENGINEOS.md](docs/ENGINEOS.md#product-identity-spoofing)). Version cells
hold the exact firmware version booted, `?` for untested, `–` where that major
version was never released for the device.

|                         | Code  | SOC    | Signed FW | Arch  | 1.x | 2.x | 3.x | 4.x     | 5.x   |
|-------------------------|-------|--------|-----------|-------|-----|-----|-----|---------|-------|
| Denon DJ Prime 2        | JC16  | RK3288 | N         | armv7 | ?   | ?   | ?   | ?       | ?     |
| Denon DJ Prime 4        | JC11  | RK3288 | N         | armv7 | ?   | ?   | ?   | 4.3.0   | ?     |
| Denon DJ Prime 4+       | JC11S | RK3288 | Y         | armv7 | –   | –   | ?   | ?       | ?     |
| Denon DJ Prime GO       | JP11  | RK3288 | N         | armv7 | ?   | ?   | ?   | 4.3.0 ! | ?     |
| Denon DJ Prime GO+      | JP11S | RK3288 | Y         | armv7 | –   | –   | –   | ?       | ?     |
| Denon DJ SC5000 Prime   | JP07  | RK3288 | N         | armv7 | ?   | ?   | ?   | ?       | 5.0.4 |
| Denon DJ SC5000M Prime  | JP08  | RK3288 | N         | armv7 | ?   | ?   | ?   | ?       | ?     |
| Denon DJ SC6000 Prime   | JP13  | RK3288 | N         | armv7 | ?   | ?   | ?   | ?       | ?     |
| Denon DJ SC6000M Prime  | JP14  | RK3288 | N         | armv7 | ?   | ?   | ?   | ?       | ?     |
| Denon DJ SC Live 2      | JP20  | RK3288 | Y         | armv7 | –   | ?   | ?   | ?       | ?     |
| Denon DJ SC Live 4      | JP21  | RK3288 | Y         | armv7 | –   | ?   | ?   | ?       | ?     |
| Numark Mixstream Pro    | NH08  | RK3288 | Y         | armv7 | –   | ?   | ?   | ?       | ?     |
| Numark Mixstream Pro+   | NH08S | RK3288 | Y         | armv7 | –   | ?   | ?   | ?       | ?     |
| Numark Mixstream Pro GO | NH10  | RK3288 | Y         | armv7 | –   | –   | ?   | ?       | ?     |
| RANE SYSTEM ONE         | RMZ2  | RK3588 | Y         | arm64 | –   | –   | –   | 4.6.0   | 5.0.4 |

SC5000 on 5.0.4 boots to a rendered UI and binds a virtual control surface; audio is
what it still lacks — see the support matrix above. Its image (`JP07-JP08-JP11`) also
carries `JP08` and `JP11`; both are selectable with `PRODUCT_CODE=` on the rootfs
build but neither has been booted. `PRODUCT_CODE=` works on the arm64 builder too,
defaulting to `RMZ2`. `!` on Prime GO 4.3.0 marks a known issue rather than a clean pass.

## Emulated MPCs

Same shape as the table above. **Code** is again the `inmusic,product-code` value,
read here from the `model` and `inmusic,product-code` properties of the
`/boot/rk3288-*.dtb` files the firmware ships. Version cells hold the exact firmware
version booted, `?` for untested. The Gen 1 MPCs all ship from one image covering
eight models, so a version column is the *image* version.

|               | Code  | SOC    | Signed FW | Arch  | 2.x | 3.x     |
|---------------|-------|--------|-----------|-------|-----|---------|
| MPC X         | ACV5  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC X SE      | ACV5S | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC Live      | ACV8  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC Live Mk 2 | ACVB  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC One       | ACVA  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC One+      | ACVA2 | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC Key 61    | ACVM  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |
| MPC Key 37    | ACVR  | RK3288 | 3.x only  | armv7 | ?   | 3.9.1 * |

`*` — the shared 3.9.1 image boots to a rendered UI, but no per-model identity is
asserted: unlike Engine, the MPC build spoofs no product code (there is no `dtshim`
in `build_mpc_rootfs.sh`, and `/usr/bin/az01-launch-MPC` selects nothing), so the
pass covers the image all eight models ship from rather than an individual model.

**Signed FW is a version boundary here, not a per-model one.** Every 2.x image
begins `d00dfeed` — a DTB wrapper around the whole file, which `binwalk` alone
cannot unpack, so those need `mpcimg` first. Every 3.x image begins `415a3031`
(ASCII `AZ01`), the signed container `binwalk` reads directly. That is also why the
2.x column is untested: `new_instance.sh` cannot ingest those images as they stand.
The latest 2.x is `2.15.1.1`.

All eight models appear in both majors, joining at different points — `ACV5` and
`ACV8` from 2.2.0, `ACVA`/`ACVB` from 2.7.2, `ACVM` from 2.10.0, `ACV5S` from
2.11.1, `ACVA2` from 2.11.7, `ACVR` from 2.13.0.14 — which is visible in the image
filenames, each listing the codes it serves.

One caveat on names: `ACV5` and `ACV8` carry generic `InMusic MPC ACV5`/`ACV8` model
strings rather than marketing ones, so the split of those two specifically between
MPC X and MPC Live follows
[BUILD_MPC.md](scripts/build_scripts/BUILD_MPC.md)'s product list rather than the
firmware. The other six name themselves outright (`Akai Professional MPC One`, and
so on). The `az01`/`az05` platform split the dtb filenames show is dropped here to
keep the columns parallel with the controllers table; `ACVA2`, `ACVR` and a second
`ACVM` variant are az05, the rest az01.

## Other inMusic devices

The same `az0x` base distribution carries several product lines this project has
**no rootfs builder for yet** — `new_instance.sh` rejects them, since it matches on
`/usr/Engine` and `/usr/bin/MPC` and these have neither. Recorded here because the
firmware is in the same archive and the extraction recipe is the same:

| Line | Codes | Notes |
|------|-------|-------|
| Akai Force | `ADA2` | Standalone groovebox, closest sibling to the MPC line |
| Akai MPC Gen 2 | `MPC-GEN2` | arm64 — a different pipeline; the MPC build rejects it up front |
| Akai MPC Sample | `AC50` | |
| HeadRush | `HV01` (Core), `HV03` (VX5), `HG02` (Gigboard), `HG03` (Looperboard), `HG04` (MX5), `HG06` (Prime), `HG12` (Flex Prime), `MG01` (Pedalboard) | Guitar FX/amp modellers |
| Alesis drum module | `LDMD`, `LDMF` | Shipped as `drummodule-*-update.img` |

**"Evil" is HeadRush.** The HeadRush application binary is built around a namespace
called `Evil` — `EvilApp`, `EvilGui`, `EvilClient`, `EvilDB`, `EvilAPI`,
`EvilAppRunner`, `Evil::BoardModes` — with its database tables named for
amp-modeller concepts (`RigDescriptorsList`, `SetlistDescriptorsList`). The binary
also identifies itself as `headrushfx`. It is an internal codename, not a separate
product line: anything labelled "Evil" in an extracted rootfs is the HeadRush
application. Confirmed against `HV01` (HeadRush Core).

**"Looper" is the Looperboard.** `HG03` is the HeadRush Looperboard, and
`LooperController` lives inside that same `Evil` binary — the looper is a mode of
the shared HeadRush application rather than its own codebase, which is why one
firmware lineage covers both the pedal-style units and the Looperboard.

| Denon DJ Prime 4 | RANE SYSTEM ONE |
|------------------|-----------------|
| ![Emulated Prime 4, Engine OS 4.3.0 About Screen](images/DENON_DJ_PRIME_4_ABOUT_430.jpg) | ![Emulated SYSTEM ONE, Engine OS 4.6.0 About Screen](images/RANE_SYSTEM_ONE_ABOUT_460.png) |
| ![Emulated Prime 4, Engine OS 4.3.0 Playlist Screen](images/DENON_DJ_PRIME_4_PLAYLISTS_430.png) | ![Emulated SYSTEM ONE, Engine OS 4.6.0 Playlist Screen](images/RANE_SYSTEM_ONE_PLAYLISTS_460.png) |
