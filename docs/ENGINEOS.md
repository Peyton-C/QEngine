# Engine OS Overview
Engine OS is a buildroot based linux distro developed by InMusic. Engine OS offically targets the armv7/armhf Rockchip RK3288 and arm64 Rockchip RK3588 based stand-alone controllers made by Denon DJ, Numark and RANE.

Everything below was observed on the armv7/RK3288 lineup (Prime/SC/Mixstream controllers). The arm64/RK3588 RANE SYSTEM ONE is a separately-explored target that diverges from it in several ways, see [arm64 / RK3588](#arm64--rk3588-rane-system-one) at the end of this document.

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
- **Known defect in older (2.4.0-era) builds**: `evdevtouch` loads but is never instantiated — touch input silently does nothing, confirmed with both real touch hardware and synthetic uinput input. Fixed by 4.3.0, where `evdevtouch` works correctly with both synthetic and real touch input (via vnctouchbridge`, see [BUILDING.md](BUILDING.md)).
- Rotation is read from `/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation`, a raw big-endian `<u32>` cell compensating for a physically rotated panel (e.g. `0x10e` = 270° on real Prime Go hardware). Under emulation this should be forced to `0` — there's no physical panel to correct for, and the "correct" real-hardware value does not fix Prime Go's known rendering issue (below).
- Most small UI icons/images fail to render; cover art and the boot/shutdown logo do not. Root cause: Qt's image-format plugins (`libqjpeg.so`,`libqsvg.so`, etc.) all depend on the real proprietary `libmali.so.14.0`, which this project replaces system-wide with Mesa (see [BUILDING.md](BUILDING.md)). Cover art and the logo seem to be unaffected.
- **Open issue — Prime Go Display**:  Any output with the Prime Go PID results in only the upper left quater of the display being visible, seems to be unique to the Prime Go.

## MIDI
- External MIDI/audio controllers are detected via a generic ALSA-sequencer name scan (`air.devicemanager.midi.*`), not a VID/PID allowlist — confirmed working with multiple different real controllers over USB passthrough.
- Requires ALSA USB audio/MIDI-class kernel modules, which a generic cloud kernel doesn't ship by default: `snd-hwdep`, `mc`, `snd-seq-device`, `snd-seq`, `snd-rawmidi`, `snd-seq-midi-event`, `snd-ump`, `snd-usbmidi-lib`, `snd-seq-midi`, `snd-usb-audio` (load in that order).
- PCM audio opens only at each device's exact fixed hardware format (no negotiable range).
- **Unresolved**: Engine logs `"The port isn't opened for Midi::Out::<name> MIDI 1"` and a failure to fetch the audio device from an empty-string device name — reproducible even with zero MIDI hardware attached, so it's an environment-level gap, not controller-specific. `DeviceConfiguration.json`'s `AudioDevices` field does not appear to be the live source Engine reads from at startup (editing it directly has no effect). The empty-device-name symptom specifically is root-caused on the RMZ2/5.0.4 build — see [Audio playback: silent failure, root-caused via decompilation](#audio-playback-silent-failure-root-caused-via-decompilation) below; `DeviceConfiguration.json` not mattering lines up with that finding, since the actual gate is a C++ type check, not a config value.
- Engine exposes a modular QML-based control-surface system (`:/ControlSurfaceModules/`, `GlobalAssignmentConfig.qml`) rather than hardcoded per-device logic — not compiled into the main binary, likely in a separate QML plugin or on-disk `qmldir`.

## Systemd services disabled under emulation
- `engine.service` — auto-launches Engine at boot with none of the environment (QPA platform, touch device, preloaded shims) needed under emulation.
- `az01-libmali-setup.service` — reads Mali GPU hardware revision from a sysfs path that doesn't exist under virtio-gpu; fails harmlessly at its first line, disabled for cleanliness.

## Root access
Real firmware ships `/etc/shadow` with a real root password hash, blank it per image to allow passwordless root login on the serial console (see [BUILDING.md](BUILDING.md)).

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
[shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.c](../shims/rk3588/touchbridge_rmz2/touchbridge_rmz2.c) translates the tablet's real pointer events into a synthetic `uinput` multitouch device the same way `vnctouchbridge` does, just without needing to parse any VNC/RFB protocol — there's no "Engine's own VNC server" to proxy here since `eglfs` has no VNC QPA plugin at all; QEMU itself is the VNC server, and it already injects real client clicks straight into the `usb-tablet` device as normal kernel evdev events, so the bridge only has to read the right `/dev/input/eventN` node directly.

Which number that is isn't stable — same class of fragility as the IRQ shim above, since `/dev/input/eventN` numbering is assigned in device-registration order and shifts whenever the QEMU device list changes (confirmed directly: trimming a passed-through USB controller out of a launch script shifted the tablet from `event2` down to `event0`, and `touchbridge_rmz2.service` originally hardcoded `event2` in its `ExecStart`). Fixed the same way as the IRQ shim: `touchbridge_rmz2.c` now scans `/dev/input/event*` at startup and picks whichever one's `EVIOCGNAME` matches QEMU's tablet and actually reports `ABS_X`/`ABS_Y`, rather than assuming a fixed number — `touchbridge_rmz2.service`'s `ExecStart` now only passes screen width/height, no device path.

### GPU: open-source Panthor, not proprietary libmali
RK3588 ships a Mali-G610 (Valhall), and — unlike RK3288's Mali-T760 — this firmware uses the mainlined, open-source **Panthor** DRM driver (`panthor.ko`, `panthor_dri.so`) plus `panfrost_dri.so`, not a proprietary `libmali.so` blob. There's no libmali-for-Mesa substitution to do here at all, which is a simplification versus the RK3288 setup — whatever driver `virtio-gpu` needs under QEMU is a separate question from what runs on real hardware, since Panthor only matters for the physical SoC.

### systemd services differ (`az0x-*`, not just `az01-*`)
Service naming follows an `az0x-*`/`az04-*` convention roughly analogous to RK3288's `az01-*` naming, but isn't a drop-in match — e.g. there's no `az01-libmali-setup.service` equivalent (no libmali to set up at all, see above), and `engine.service` here is `Type=forking` running a wrapper (`usr/Engine/Scripts/runengine` → `usr/Engine/Scripts/engine`, which calls `setup-prerequisites.sh` and `encrypt-fs.sh` before actually exec'ing `/usr/Engine/Engine`) rather than launching the binary directly.

### Devicetree/IRQ crashes under QEMU — fixed
With no real devicetree (QEMU's `virt` machine synthesizes its own, with none of the `inmusic,*` properties), `usr/Engine/Scripts/engine` reads an empty product code, and Engine aborts (`SIGABRT`, `air.planck.config: Unable to find product "" in config map!`) — crash-looping via `engine.service`'s `Restart=on-failure`. Same root cause and same fix shape as the RK3288 devicetree spoof ([Product identity spoofing](#product-identity-spoofing) above): an aarch64 rebuild of `dtshim.c` (`shims/rk3588/dtshim/dtshim_rmz2.c`) with `RMZ2` in place of `JC11`/`JP11`. Separately, Engine also hard-throws if it can't find `/proc/interrupts` lines by name for six real-hardware components that don't exist under QEMU — fixed by the same shim remapping `/proc/interrupts` to fake content. That content used to be a static pre-generated file, which meant regenerating it by hand every time the QEMU device list changed shifted real IRQ/MSI-vector numbering out from under it (a recurring, fragile failure mode). `dtshim_rmz2.c` now generates it dynamically instead — reads the real `/proc/interrupts` at runtime, filters to verified-writable `MSI`/`Edge` IRQs (matches both plain `MSI` and `ITS-MSI` — the controller label varies by kernel build, confirmed directly switching kernels), relabels six of them with the fake names Engine expects — so it self-heals across device-list changes instead of needing manual regeneration. The old static file still exists as a last-resort fallback for the rare case the dynamic path can't find any usable candidates.

That alone wasn't sufficient, though: a probe-then-use approach still has a real gap, confirmed directly. The candidate IRQ passes a genuine read-then-write-back probe at `/proc/interrupts`-read time, then fails Engine's own affinity write moments later with the exact same `EPERM` — because Engine doesn't set affinity itself, it shells out (`sh -c "echo ... > /proc/irq/N/smp_affinity_list"`), and the underlying real MSI vector our fake name landed on can transition from freely-reaffinitizable to pinned once its actual owning driver (whatever real virtio device that vector belongs to) finishes its own initialization — on its own schedule, independent of when Engine gets around to setting affinity for our fake name. Since the whole device is already fictional, the shim now also intercepts `write()` and fakes success for `smp_affinity`/`smp_affinity_list` writes targeting the specific real IRQ numbers it mapped — propagated to Engine's shelled-out child via an environment variable (`DTSHIM_FAKE_IRQS`), the same way `LD_PRELOAD` itself reaches that child, since the child is a fresh `exec` of `/bin/sh` and never itself reads `/proc/interrupts` to populate the mapping on its own. See [BUILDING.md](BUILDING.md#status) and [shims/rk3588/dtshim/fake-dt-rmz2/README.md](../shims/rk3588/dtshim/fake-dt-rmz2/README.md) for the full mechanism.

### Audio playback: silent failure, root-caused via decompilation
With display, touch, and MIDI (real bidirectional SysEx with a Denon
MC6000MK2 over USB passthrough) all working on Engine 5.0.4/RMZ2, audio
never plays: a track loads (cover art appears) then silently reverts to
"No Track Loaded" — no error dialog, no crash. Reproducible on every
codec/container tested (flac/mp3/m4a/wav), on both the built-in demo
library and a real Engine Library on a passed-through USB flash drive,
across the emulated `ich9-intel-hda`/`hda-duplex` ALSA path and a
passed-through real Bluetooth adapter (BlueZ negotiates a real SBC A2DP
session, confirmed active) — ruling out track format, database, and
output backend as the variable.

**Where the audio actually stops.** `LD_PRELOAD` instrumentation
(intercepting `open`/`openat`/`read`/`pread64` to track file descriptors,
plus `/proc/<pid>/task/<tid>/wchan` on every Engine thread) shows the full
decode pipeline completing normally — exact byte-count match between
bytes read and the track's real duration/bitrate — and IPC to the
`OfflineAnalyzer` helper processes staying healthy throughout. The
`AudioRenderer2`/`AudioRenderer3`/`AudioRenderer4` and `BluetoothOutput`
threads are permanently parked in `futex_wait_queue` — decoded audio is
produced but nothing ever signals these threads to consume and play it,
regardless of which output backend is active.

**Why, from decompiling `Engine.bin`** (Ghidra, see
[BUILDING.md](BUILDING.md#audio-playback-decompiling-enginebin) for the
toolchain notes): `airHost::updateAudioDeviceChanged(bool)` is the source
of the `"Failed to fetch the audio device \"\" from the device manager"`
warning that's been visible in the logs since the very first boot. Its
logic doesn't do a name lookup at all:

- The persisted output-device preference can be the literal sentinel
  string `"Default Device"`, meaning "auto-pick." (Real hardware has
  exactly one physical output, so there's normally nothing to pick — this
  also explains why `State/`/`UserProfiles/` are empty on-disk: no
  per-user device choice is ever persisted.)
- The auto-pick path walks Engine's enumerated device list — objects of
  a base class internally named `airDevice` — and calls
  `dynamic_cast<airAudioDevice*>()` on each one. Only objects that are
  actually instances of the `airAudioDevice` subclass are eligible
  outputs.
- **Nothing in the emulated device list ever gets classified as
  `airAudioDevice`** — not the HDA card, regardless of what it's named
  (renaming the emulated card's ALSA `id` to `RMZ2` to match the real
  hardware's `simple-audio-card` name, on the theory Engine looked things
  up by name, made no difference — confirming the gate is a type check,
  not a name match). With the candidate list empty, the code falls
  through with the device name string still at its zero-initialized empty
  value, which is the exact `""` seen in the log.

So the chain is: no object ever gets constructed as `airAudioDevice` for
any device QEMU can present → the auto-pick logic never finds a valid
output → Engine never attaches a real sink to the playback pipeline →
`AudioRenderer*`/`BluetoothOutput` have nothing to wait for and block in
`futex_wait_queue` forever. This is consistent across every backend
tested because the gate is upstream of ALSA/Bluetooth selection
entirely.

**Unresolved**: what exactly gates `airAudioDevice` construction — a
hardcoded check for the onboard `az04-codec` hardware specifically (see
[BUILDING.md](BUILDING.md#the-az04-codec-driver-exists-and-is-unusable-here)
for why that specific hardware can't be reproduced under QEMU), a
broader "physical/onboard only, no USB-class audio" policy, or something
else. ~20 functions reference the `airDevice`/`airAudioDevice` RTTI in
this binary; the actual constructor hasn't been isolated yet.

**Ruled out along the way**: the on-disk rootfs itself has no config file,
ALSA UCM profile, or shell script that names or selects an audio device —
confirmed by direct inspection (no `/usr/share/alsa/ucm2` at all,
`setup-prerequisites.sh`/`check_device_by_platform.sh` are unrelated —
IRQ/priority tuning and storage-media detection respectively — and
`az04-info` needs real hardware sysfs that doesn't exist under QEMU). The
gate really is compiled entirely into `Engine`'s own logic. Separately,
`/usr/Engine/AssignmentFiles/PresetAssignmentFiles/RMZ2/f_midi-0_Device.qml`
confirms with real on-disk evidence what was only inferred earlier from
the binary's strings: the `hw:UAC2Gadget`/`f_midi` USB-gadget MIDI
endpoint is for **"Computer Mode"/"Hybrid Mode"** — an external computer
plugged into SYSTEM ONE's USB port using it as a MIDI+audio interface for
its own DJ software (`Planck.getProperty("/Engine/Mixer/Channel1/Computer")`,
`isHybridModeHostConnected`) — a completely different path from Engine's
own onboard standalone playback, not a lead worth chasing further for
this bug. The sibling `RMZ2_Controller_Device.qml` in the same directory
is the physical control-surface MIDI protocol (SysEx manufacturer/product
ID handshake, motor/fader calibration, OLED pad displays) — also
unrelated to audio output device selection.

`/usr/lib/az0x-usb-gadget/rmz2.scheme` (the ConfigFS gadget descriptor
actually presented to a connected computer) confirms this from the USB
descriptor side too: an 8-channel (`c_chmask`/`p_chmask = 255`),
44.1kHz/32-bit `uac2_audio` function plus a 2-in/2-out `midi_midi`
function, `idVendor = 0x1cc5` (InMusic), `idProduct = 0x1027` (`0x27` —
matches the MIDI SysEx `productId` in `RMZ2_Controller_Device.qml`,
confirming both describe the same Computer-Mode product identity).
Checked every other `az*`-prefixed binary on the rootfs
(`az0x-hwctl`, `az0x-info`/`az04-info`, `az0x-migrate`, `az01-update`,
`az01-signed-fs`, `az01-bootloader-sig`, `az0x-splashctl`, `az01-image`,
`az0x-gadget-mac`, `az0x-setup-hostname`, `az01-coredump`,
`az01-pwrbtn`, `az01-script-runner`) — none of them touch onboard audio
device selection either (`az0x-hwctl` only exposes a `typec` USB-C
port-role property; the rest are update/signing, boot-splash, crash
handling, or networking identity). The gate is fully self-contained
inside `Engine`'s own binary — there is no external helper tool or
config file anywhere on this rootfs that names or selects the onboard
audio device.
