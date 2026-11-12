# Setup everything required to emulate an Akai MPC
Requires docker, qemu, python3, e2fsprogs.

1. [Download MPC firmware from Akai](https://www.akaipro.com/downloads-and-support/downloads/firmware/mpc/). The Gen 1 `.img` covers the RK3288 models (MPC X, X SE, Live, Live 2, One, One+, Key 61, Key 37) and is armv7. The Gen 2 `.img` covers the RK3588 models (MPC One Gen 2, Key 37 Gen 2, Key 61 Gen 2, XL, Live III, Force Gen 2) and is arm64.
2. Build the rootfs with `build_mpc_rootfs.sh --firmware PATH_TO_MPC_UPDATE.img`, prefixed with `ARCH=arm64` for Gen 2.
3. Get the kernel and initrd with `get_kernel.sh --arch armhf`, or `--arch arm64` for Gen 2.
4. Make a /data disk with `make_disk.sh --family mpc`, or `--family engine` for Gen 2, which uses the same `data` and `factory` partitions as RK3588 Engine OS.
5. Boot with `DEVICE=mpc ARCH=armhf scripts/qemu/run_qemu.sh`, or `ARCH=arm64` for Gen 2.

Or let one command do all of it, with the device family and architecture identified from
the firmware — see [INSTANCES.md](INSTANCES.md):

```sh
scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --firmware PATH_TO_MPC_UPDATE.img
scripts/qemu/run_instance.sh --name mpc-3.9.1
```

Touch works out of the box: the rootfs build installs `touchbridge_mpc` and holds `acvs.service` back until its touchscreen exists, since MPC looks for touchscreens only when it starts. MPC only responds to a real touchscreen, and QEMU's tablet presents as an absolute mouse, so the bridge re-emits it as a uinput multitouch device. Taps need to be held briefly, as an instant press and release from a scripted VNC client does not register.

## Control surface
The rootfs build preloads `mpcsurface` into MPC, which takes it for its own panel: the pads, buttons, dial and Q-Links. `mpcsurface` picks which panel to be from the product code, and MPC names itself after the panel it finds, so an `ACVA3` guest comes up as an MPC One.

Write commands to `/run/mpcsurface.fifo` on the guest, one per line:

```sh
echo 'button play' > /run/mpcsurface.fifo
```

| Command | What |
| --- | --- |
| `button <name> [ms]` | Press and release a button |
| `hold <name>`, `release <name>` | Keep a button down, as for `shift`, and let it go |
| `pad <1-16> [velocity] [ms]` | Strike a pad, pad 1 at the bottom left |
| `turn dial <steps>`, `turn q1 <steps>` | Turn the data dial or Q-Link 1 to 4, negative for anticlockwise |
| `touch q1 [ms]` | Touch a Q-Link without turning it |
| `list` | Log the button names this product has |
| `press <ch> <note> [ms]` | Raw note on and off, channels from 0 |
| `on <ch> <note> [vel]`, `off <ch> <note>` | Raw note on, raw note off |
| `cc <ch> <cc> <value>` | Raw control change |
| `sysex <hex bytes>` | Raw SysEx |

Buttons have names only on `ACVA3`. On any other product use `press`, the numbers differ between panels. `pad` covers the sixteen-pad products and not Force.

| `ACVA3` buttons | |
| --- | --- |
| Transport | `play` `stop` `rec` `overdub` `play-start` `locate` |
| Editing | `menu` `main` `shift` `undo` `copy` `erase` `note-repeat` `full-level` `16-level` `tc` `automation` `plus` `minus` `dial` |
| Pad banks | `bank-a` `bank-b` `bank-c` `bank-d` |
| Modes | `sample-edit` `pad-mixer` `track-mixer` `program-edit` `song` `arrange` `grid` `next-seq` `track-mute` `pad-color` `pad-perform` `xyfx` |
| Q-Link modes | `qlink-project` `qlink-track` `qlink-pad-scene` `qlink-pad-param` `qlink-screen` |

The names are what each button did on screen or to its lamps, with no real panel to check them against. A button that changed neither has no name.

A raw command can send what no panel would. On `ACVA3` a note on for `0x58` on channel 0 stops MPC listening to the surface until `acvs.service` is restarted.

To see what MPC sends the panel, such as the lamp for a button with no name yet, set `MPCSURFACE_VERBOSE=1` and `MPCSURFACE_LOG=/data/surface.log` in a drop-in for `acvs.service`.

Tried on arm64, `ACVA3`, 3.10.0 only. The other Gen 2 codes and all of Gen 1 are built with it and untested. The first time MPC met the surface the guest rebooted itself once, and not again.

## Audio
MPC plays through the host's sound output. It opens its internal card by udev path, `/dev/snd/by-path/platform-sound`, and the build points that path at `snd-dummy`, which `qengine-mpc-audio.service` loads before `acvs.service`. `alsashim` is preloaded into MPC and copies the first pair of everything it plays onto QEMU's HDA card, which `run_qemu.sh` gives the `mpc` family as it does Engine. Inputs are silent.

| Card | Why not |
| --- | --- |
| QEMU's HDA | Moves samples every 10 ms, and on macOS has no capture; MPC will not run on a playback-only card |
| `snd-aloop` | Moves in 4 ms jiffies, and MPC's streams stall a tick after starting |
| Either, with longer periods | MPC crashes within a second on any period but its own 128 frames |

Debian builds `snd-dummy` for neither architecture, so `get_kernel.sh` builds it from the matching stable kernel source and puts it in the initrd, loaded only when an MPC guest asks for it. A kernel fetched before that has no `snd-dummy`, and MPC shows "No internal audio device could be found!" until `get_kernel.sh` is run again.

The output runs about 30 to 80 ms behind MPC, and the two cards' clocks drift, which can cost a short tick every so often. Run with `QEMU_AUDIO=off` and MPC still plays to the dummy card, unheard.

Heard on arm64, `ACVA3`, 3.10.0, on macOS through CoreAudio. Gen 1 builds the same way and is untested. MPC Sample is covered under its own heading.

## Gen 2
Gen 2 runs the same MPC application on RK3588, and needs the same small shim stack as Gen 1: it drives KMS through libdrm with no Mali or EGL, and links no TEE client, so of arm64 Engine's shims only `alsashim` applies, for sound. `build_mpc_rootfs.sh` builds `dtshim` with the RK3588 table, which already serves everything the Gen 2 dtbs declare at the root: `inmusic,product-code`. The default product code is `ACVA3`.

| Model | Code |
| --- | --- |
| MPC One (Gen 2) | `ACVA3` |
| MPC Key 37 (Gen 2) | `ACVR2` |
| MPC Key 61 (Gen 2) | `ACVM2` |
| MPC XL | `ACVC` |
| MPC Live III | `ACVG` |
| Force (Gen 2) | `ADA2S` |

The dtbs also set `chosen` flags per model that QEngine does not serve: `acvs-loopback-content`, `emmc-synths` and `nvme-fitted`. Without them the guest skips `storage.mount`, `content.mount` and `nvme.mount`. Where MPC then keeps user content is untested.

`az0x-info` reports "No data available" on Gen 2, even with `dtshim` preloaded, and MPC logs `Unhandled board platform: <unknown>`. Neither stops the UI.

To build and run several devices or firmware versions side by side, see
[INSTANCES.md](INSTANCES.md).

## MPC Sample
QEngine boots MPC Sample 1.3.0 as `AC50` on arm64, using the RK3566 devicetree table and a 320×240 virtual display. Point `--firmware` at the desktop updater zip:

```sh
scripts/build_scripts/new_instance.sh --name mpc-sample-1.3.0 --product-code AC50 --firmware "MPC Sample 1.3.0 Updater.app.zip"
scripts/qemu/run_instance.sh --name mpc-sample-1.3.0 --display vnc
```

| Item | Status |
| --- | --- |
| Application | `ac50.service` reaches the sample trim screen |
| Display | Landscape, software rendering, no SPI panel emulation |
| Storage | `/data` and `/factory` mount with the existing arm64 disk layout |
| Factory content | Separate content partition absent from the updater, not supplied by QEngine; bundled temporary project loads |
| Audio | Plays through the host, inputs silent, see below |
| Controls | `mpcsurface` as its panel, see below; the mouse does nothing, as the device has no touchscreen |
| TEE | No additional shim needed to reach the UI |

MPC Sample uses physical buttons rather than a touchscreen, so the build skips touchbridge. Only arm64, `AC50`, firmware 1.3.0 has been booted.

### Sample control surface
The Sample's panel is a microcontroller on a UART, which the app finds as a sound card named "Control Surface". `mpcsurface` is that card when the product code is `AC50`, and takes the same fifo commands as on MPC, with `set` for the knobs:

```sh
echo 'pad 6 120' > /run/mpcsurface.fifo
echo 'set volume 127' > /run/mpcsurface.fifo
```

| Command | What |
| --- | --- |
| `button <name> [ms]`, `hold`, `release` | `sample` `seq` `pad-fx` `knob-fx` `shift` `pad-bank` `erase` `note-repeat` `screen-1` `screen-2` `screen-3` `chop` `mute` `loop` `16-levels` `sample-select` `tap-tempo` `minus` `plus` `sample-rec` `seq-rec` `stop` `play` `dial` |
| `pad <1-16> [velocity] [ms]` | Strike a pad |
| `turn dial <steps>` | The data dial |
| `set <k1\|k2\|k3\|volume\|fader> <0-127>` | Move a knob or the fader to a position |

MAIN VOLUME is the Sample's software volume, and the app learns it only from the panel, so `mpcsurface` reports 100 when the app starts. Set `MPCSURFACE_VOLUME` in a drop-in for `ac50.service` to start elsewhere. K1 to K3 and the fader are not reported, since the app takes them as a move on the open page.

The button names and protocol come from [virtual-mpc-sample](https://github.com/orrpunk-art/virtual-mpc-sample), which checked them against a real panel. Pads, the screen buttons, `seq`, `play`, `sample-select`, the dial and volume were tried here. The firmware tool that runs before the app still reports no panel, which does not stop it.

### Sample audio
The Sample plays the same way MPC does: `snd-dummy` as `platform-sound`, and `alsashim` copying its first pair, the speaker and line out, to QEMU's HDA card. It opens that card as four channels of S32_LE at 44.1 kHz, which `snd-dummy`'s own card does not offer, so `get_kernel.sh` adds a `qengine-ac50` model that the Sample's guest loads. A kernel fetched before that has the model missing, and the Sample gets no sound until `get_kernel.sh --arch arm64` is run again. Heard on macOS through AirPods Pro 3 once QEMU's CoreAudio output ran at 48 kHz, which the launchers now do; at QEMU's default 44.1 kHz it clicked.

## Alesis drum module
The drum module's `/usr/bin/DrumModule` is MPC's codebase on Gen 1's platform, armv7 with the same `/data` PARTUUID, and `build_mpc_rootfs.sh` builds it. It reads the app off the rootfs and preloads `dtshim` into `alesis-drummodule.service` in place of `acvs.service`. The default product code is `LDMD`.

```sh
scripts/build_scripts/new_instance.sh --name drummodule-1.5.2.3 --firmware drummodule-1.5.2.3-update.img
```

The module keeps its kit library and everything it writes on an internal SD card at `/sdcard`, with a writable overlay on `/sdcard/bfd`. The rootfs is mounted read-only. `new_instance.sh` makes the card as `sdcard.img`, a blank ext4 that `make_disk.sh --family internal-sd` formats, and `run_qemu.sh` attaches it as a third drive. The build makes the guest use it:

| What | Why |
| --- | --- |
| `/etc/systemd/system-generators/ldmx-content-*-generator` | The vendor generators pick the storage layout from the product code with a plain `cat`, before any unit runs, so they see none and set up LDMF's. Each wrapper runs the vendor script with `dtshim` preloaded. |
| `90-qengine-internal-sd.rules` | Names the card `/dev/disk/by-path/platform-ff0c0000.dwmmc-part1`, the RK3288 SD path `sdcard.mount` and the app's fsck check look for, matched by the UUID `make_disk.sh` formats it with. |
| `qengine.conf` drop-ins | Clear the `chosen/inmusic,internal-sd-fitted` condition on `sdcard.mount`, `sdcard-bfd.mount`, the card's device unit and its fsck, which systemd tests without `dtshim`. |

The overlay's lower layer is `/usr/share/Alesis/ldmd-content-update`, which carries the factory kits and presets. The full sample library ships on the card at the factory and is in no update image, and with no audio device it is untested what a kit plays without it. Without the card the app stops at "File system problem".

`LDMF` is refused. It keeps its content on an eMMC partition, `content.mount` with PARTUUID `4054a70b-cc6a-4ac8-9a97-2a716b05bbb4`, that no QEngine disk provides.

The launcher runs `az01-firmware-tool` against the pad controller's firmware on every start. With no controller it reports MCU version 0.0.0.000 and carries on.

## Notes

- Used directly as above, the rootfs is written to `build/rootfs_out.img` — the
  same path the arm64 build uses, so building one target overwrites the other.
  Use an instance (see INSTANCES.md) to keep several targets side by side.
- Plain FIT images (`d00dfeed`: MPC 2.x, Prime GO 4.3.0 and earlier, HeadRush Pedalboard and Looperboard) go through `extract_fit_rootfs.py`, since binwalk 3 reads each as a single DTB. `AZ01` (MPC Gen1) and `AZ0x` (MPC Gen2) go through `extract_az01_rootfs.py` and `extract_az0x_rootfs.py`. A Mac `.app` zip or Windows `.exe` updater is unpacked by `extract_updater.py` first.
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
