# Engine OS Overview
Engine OS is a buildroot based linux distro developed by InMusic. Engine OS offically targets the armv7/armhf Rockchip RK3288 and arm64 Rockchip RK3588 based stand-alone controllers made by Denon DJ, Numark and RANE.

Everything below was observed on the armv7/RK3288 lineup (Prime/SC/Mixstream/LC controllers). The arm64/RK3588 RANE SYSTEM ONE is a separately-explored target that diverges from it in several ways, see [arm64 / RK3588](#arm64--rk3588-rane-system-one) at the end of this document.

## Filesystem layout
- Engine binary: `/usr/Engine/Engine` (launched with `-d0`), Qt libs at `/usr/qt/lib`.
- Per-user app data: `/data/AppDataUser/AIR Music Technology/Engine/`
  - `DeviceConfiguration.json` — audio/MIDI device selection.
  - `State/PlanckClientState.json`, `PlanckEngineState.json`, `PlanckGuiState.json` — runtime state.
  - `CrashReportDB/` — crashpad + Sentry native crash reporting, runs as a separate process alongside Engine on every launch.
- Per-product-spoof user settings (Qt `.conf`/INI, not JSON):
  `/data/.config/{jc11,jp11}.user.settings/AIR Music Technology/Engine.conf` —
  onboarding progress, streaming-service toggles, analytics UUID, ignored
  update versions. Does **not** hold audio/MIDI device selection.
- Hardware identity/config exposed via `/sys/firmware/devicetree/base/...`, read as raw binary cells, not text.

## Product identity spoofing
Product model is read from a single devicetree property:
```
/sys/firmware/devicetree/base/inmusic,product-code
```

| Value | Device    |
|-------|-----------|
| `JC11` | Prime 4  |
| `JP11` | Prime Go |

This one string drives which UI screens appear, screen-size assumptions, and
whether the battery gate (below) applies. Other identity properties faked
alongside it: `serial-number`, `inmusic,az01-pcb-rev`.

## Battery / AC detection
Battery-powered hardware (Prime Go) shows a "touch and hold the logo" prompt at boot and auto-quits after 30s if not held. Prime 4 has no battery and skips this entirely.

Two ways to bypass under emulation:
1. Spoof as `JC11` (Prime 4) — no battery, no gate.
2. Perform the hold gesture via synthetic or real touch input while spoofed as `JP11`.

Unconfirmed: QML/config properties resembling `Configuration/HasBattery` and `useFakeAC`/`fakeAC` suggest a dev override to fake AC power without a product-code swap.

## Display / rendering
- Qt 5.15.2 app. Two relevant QPA backends:
  - `vnc` — bundled `libqvnc.so`, `QT_QPA_PLATFORM=vnc:size=WxH`.
  - `evdevtouch` — generic QPA plugin, `QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/eventN`.
- **Known defect in older (2.4.0-era) builds**: `evdevtouch` loads but is never instantiated — touch input silently does nothing, confirmed with both real touch hardware and synthetic uinput input. Fixed by 4.3.0, where `evdevtouch` works correctly with both synthetic and real touch input (via vnctouchbridge`, see [BUILDING.md](docs/BUILDING.md)).
- Rotation is read from `/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation`, a raw big-endian `<u32>` cell compensating for a physically rotated panel (e.g. `0x10e` = 270° on real Prime Go hardware). Under emulation this should be forced to `0` — there's no physical panel to correct for, and the "correct" real-hardware value does not fix Prime Go's known rendering issue (below).
- Most small UI icons/images fail to render; cover art and the boot/shutdown logo do not. Root cause: Qt's image-format plugins (`libqjpeg.so`,`libqsvg.so`, etc.) all depend on the real proprietary `libmali.so.14.0`, which this project replaces system-wide with Mesa (see [BUILDING.md](BUILDING.md)). Cover art and the logo seem to be unaffected.
- **Open issue — Prime Go Display**:  Any output with the Prime Go PID results in only the upper left quater of the display being visible, seems to be unique to the Prime Go.

## MIDI
- External MIDI/audio controllers are detected via a generic ALSA-sequencer name scan (`air.devicemanager.midi.*`), not a VID/PID allowlist — confirmed working with multiple different real controllers over USB passthrough.
- Requires ALSA USB audio/MIDI-class kernel modules, which a generic cloud kernel doesn't ship by default: `snd-hwdep`, `mc`, `snd-seq-device`, `snd-seq`, `snd-rawmidi`, `snd-seq-midi-event`, `snd-ump`, `snd-usbmidi-lib`, `snd-seq-midi`, `snd-usb-audio` (load in that order).
- PCM audio opens only at each device's exact fixed hardware format (no negotiable range).
- **Unresolved**: Engine logs `"The port isn't opened for Midi::Out::<name> MIDI 1"` and a failure to fetch the audio device from an empty-string device name — reproducible even with zero MIDI hardware attached, so it's an environment-level gap, not controller-specific. `DeviceConfiguration.json`'s `AudioDevices` field does not appear to be the live source Engine reads from at startup (editing it directly has no effect).
- Engine exposes a modular QML-based control-surface system (`:/ControlSurfaceModules/`, `GlobalAssignmentConfig.qml`) rather than hardcoded per-device logic — not compiled into the main binary, likely in a separate QML plugin or on-disk `qmldir`.

## Systemd services disabled under emulation
- `engine.service` — auto-launches Engine at boot with none of the environment (QPA platform, touch device, preloaded shims) needed under emulation.
- `az01-libmali-setup.service` — reads Mali GPU hardware revision from a sysfs path that doesn't exist under virtio-gpu; fails harmlessly at its first line, disabled for cleanliness.

## Root access
Real firmware ships `/etc/shadow` with a real root password hash, blank it per image to allow passwordless root login on the serial console (see [BUILDING.md](docs/BUILDING.md)).

## arm64 / RK3588 (RANE SYSTEM ONE)
Confirmed from a signed 4.6.0 firmware extraction (see [BUILDING.md](BUILDING.md#arm64--rk3588-rane-system-one) for the QEMU bring-up itself). Everything above was observed on RK3288 hardware/builds, this device diverges from it in several ways beyond just CPU architecture.

### Product identity
Devicetree `compatible = "inmusic,rmz2", "inmusic,az04", "rockchip,rk3588"`, `model = "Rane SYSTEM ONE"`. Product code (same top-level devicetree property as RK3288, `/sys/firmware/devicetree/base/inmusic,product-code`) is
**`RMZ2`** — the arm64 equivalent of `JC11`/`JP11`. Unlike the RK3288
devicetree, there's no `serial-number` or PCB-rev property alongside it;
System One most likely provisions that per-unit data on the `/factory`
partition instead (see below), not in the devicetree.

Devicetree `compatible = "inmusic,rmz2", "inmusic,az04", "rockchip,rk3588"`,m`model = "Rane SYSTEM ONE"`. Product code (same top-level devicetree property as RK3288, `/sys/firmware/devicetree/base/inmusic,product-code`) is **`RMZ2`** for the RANE SYSTEM ONE. Unlike the RK3288 devicetree, there's no `serial-number` or PCB-rev property alongside it;
System One most likely provisions that per-unit data on the `/factory` partition instead (see below), not in the devicetree.

### Filesystem layout differs from the RK3288 overlay scheme
No `/etc`+`/var` overlay-on-eMMC at all (contrast with [Root access](#root-access) above). `/etc/fstab` mounts root plain `ro`,
and separate purpose-specific partitions — `/data`, `/factory`, plus empty `/content`/`/secure-media` mountpoints — are mounted individually by PARTUUID via static systemd `.mount` units, each gated on a oneshot
`az0x-*-mkfs` service that auto-formats the partition (`mkfs.ext4 -O encrypt`) if it isn't already labeled. `/data`'s subdirectories (`downloads`, `stems`) are then individually encrypted per-directory at every Engine launch via `fscryptctl` (`usr/Engine/Scripts/encrypt-fs.sh`, called from `usr/Engine/Scripts/engine`) — native ext4 `fscrypt`, not a LUKS/dm-crypt whole-partition scheme. `/var/lib` is bind-mounted from `/data/system/var-lib`, so it inherits whatever `/data` availability is.

### Qt6, and no VNC QPA plugin
This Engine build links Qt **6.7.2**, not the RK3288 lineup's Qt 5.15.2. More significant for this project's whole remote-display approach: its `plugins/platforms/` directory has **no `libqvnc.so`** — Qt removed the VNC QPA backend in this version range, so `QT_QPA_PLATFORM=vnc` (the mechanism [Display / rendering](#display--rendering) above and `vnctouchbridge` are both built around) isn't an option here at all. What is present: `eglfs` with KMS/GBM device integration (`libQt6EglFsKmsGbmSupport.so`),`evdevtouch` (`libqevdevtouchplugin.so`, so the touch-input mechanism itself likely still applies), `minimal`/`minimalegl` (headless, no display), and `offscreen`.

`QT_QPA_PLATFORM=eglfs` with `virtio-gpu-pci` does work under QEMU, mirrored at the QEMU level via `-vnc`/`screendump` rather than anything Qt-side — but needed a shim (`shims/dtshim/drmatomic_rmz2.c`) to get there. Two bugs, both in `eglfs-kms-gbm`'s interaction with `virtio-gpu`, not in Engine itself: it submits framebuffers in `ARGB8888`, which `virtio-gpu`'s primary plane rejects outright (it only accepts opaque `XRGB8888` — identical memory layout, so a one-field rewrite fixes it), and it drives the modeset/ flip through legacy `SETCRTC`/`PAGE_FLIP` ioctls, which the shim replaces with a real `DRM_IOCTL_MODE_ATOMIC` commit instead. Full root-cause story and the exact `dmesg` evidence are in
[BUILDING.md](BUILDING.md#status). 
Touch input works too, but needed the same kind of porting: QEMU's `usb-tablet` device is a real, correctly-classified (`ID_INPUT_MOUSE=1`) evdev pointer that Qt's built-in `evdevmouse` handler does consume, but Engine OS doesn't have any support for mice.
[shims/touchbridge_rmz2/touchbridge_rmz2.c](shims/touchbridge_rmz2/touchbridge_rmz2.c) translates the tablet's real pointer events into a synthetic `uinput` multitouch device the same way `vnctouchbridge` does, just without needing to parse any VNC/RFB protocol — there's no "Engine's own VNC server" to proxy here since `eglfs` has no VNC QPA plugin at all; QEMU itself is the VNC server, and it already injects real client clicks straight into the `usb-tablet` device as normal kernel evdev events, so the bridge only has to read `/dev/input/event2` directly.

### GPU: open-source Panthor, not proprietary libmali
RK3588 ships a Mali-G610 (Valhall), and — unlike RK3288's Mali-T760 — this firmware uses the mainlined, open-source **Panthor** DRM driver (`panthor.ko`, `panthor_dri.so`) plus `panfrost_dri.so`, not a proprietary `libmali.so` blob. There's no libmali-for-Mesa substitution to do here at all, which is a simplification versus the RK3288 setup — whatever driver `virtio-gpu` needs under QEMU is a separate question from what runs on real hardware, since Panthor only matters for the physical SoC.

### systemd services differ (`az0x-*`, not just `az01-*`)
Service naming follows an `az0x-*`/`az04-*` convention roughly analogous to RK3288's `az01-*` naming, but isn't a drop-in match — e.g. there's no `az01-libmali-setup.service` equivalent (no libmali to set up at all, see above), and `engine.service` here is `Type=forking` running a wrapper (`usr/Engine/Scripts/runengine` → `usr/Engine/Scripts/engine`, which calls `setup-prerequisites.sh` and `encrypt-fs.sh` before actually exec'ing `/usr/Engine/Engine`) rather than launching the binary directly.

### Devicetree/IRQ crashes under QEMU — fixed
With no real devicetree (QEMU's `virt` machine synthesizes its own, with none of the `inmusic,*` properties), `usr/Engine/Scripts/engine` reads an empty product code, and Engine aborts (`SIGABRT`, `air.planck.config: Unable to find product "" in config map!`) — crash-looping via `engine.service`'s `Restart=on-failure`. Same root cause and same fix shape as the RK3288 devicetree spoof ([Product identity spoofing](#product-identity-spoofing) above): an aarch64 rebuild of `dtshim.c` (`shims/dtshim/dtshim_rmz2.c`) with `RMZ2` in place of `JC11`/`JP11`. Separately, Engine also hard-throws if it can't find `/proc/interrupts` lines by name for six real-hardware components that don't exist under QEMU — fixed by the same shim remapping `/proc/interrupts` to a fake file. See [BUILDING.md](BUILDING.md#status) for both fixes in full, including the IRQ-number-drift gotcha (the mapping isn't stable across QEMU device-list/CPU-count changes and needs regenerating when it drifts).
