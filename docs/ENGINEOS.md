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
this binary; the actual constructor hasn't been isolated yet. **Ruled
out**: any dependency on `/sys/firmware/devicetree/base/*` — see
[Audio playback: a real ALSA card, reached and opened](#audio-playback-a-real-alsa-card-reached-and-opened)
below, confirmed both before and after getting a real ALSA device to
open successfully, with every devicetree read this project's tooling can
see logged and inspected.

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

### Audio playback: a real ALSA card, reached and opened

Follow-up to the decompilation findings above. Two things prompted this:
the RANE reverse-engineering wiki
([deathcamel58.github.io/denon-reverse-engineering/rane.html](https://deathcamel58.github.io/denon-reverse-engineering/rane.html))
independently confirmed the real `az04-codec` driver is a stub (no
register map, no I2C/SPI — just two optional GPIO lines, devicetree-driven
PCM capability negotiation), and a re-look at the real
`snd-soc-inmusic-az04.ko` — unstripped, unlike `Engine.bin` — via
`objdump` confirmed exactly that structurally (see
[BUILDING.md](BUILDING.md#audio-playback-decompiling-enginebin)).

**Devicetree hypothesis, tested and ruled out (twice).** Before building
anything, `dtshim_rmz2.c` was extended to log every
`open`/`open64`/`fopen`/`fopen64` call under
`/sys/firmware/devicetree/base/` or `/proc/device-tree/` — not just the
handful of paths it already remaps — to a plain file
(`/root/dtshim-dt-access.log`), so a real boot could be inspected for any
devicetree read this project hadn't already accounted for. Two full
passes (before and after the ALSA-level fix below) show the exact same
small, constant set: `inmusic,product-code` (once, by the
`runengine`/`engine` wrapper scripts' shell `cat`, and again by
`Engine.bin` itself), `serial-number` (read repeatably — dozens of times
over a session, for reasons unrelated to audio), and one harmless legacy
probe of the RK3288-era `mipi@ff960000/panel@0/rotation` path that always
misses. Nothing audio- or `az04`-related ever appears, including exactly
when a real ALSA device open succeeds (below) — the `airAudioDevice` gate
reads no devicetree property at all, at any point.

**Reimplementing `az04-codec` as a loadable module.** The real driver's
`probe()` (read via `objdump` on the unstripped `.ko`) is roughly 40 lines:
allocate a private struct, read `inmusic,playback-channels` /
`inmusic,capture-channels` / `inmusic,pcm-rates` via
`device_property_read_u32_array()` into the DAI's capability struct, grab
two optional GPIOs (`mute`, `reset`), call
`devm_snd_soc_register_component()`. Genuinely reimplementable with zero
vendor code, confirmed directly:
[shims/rk3588/az04-audio/az04_codec.c](../shims/rk3588/az04-audio/az04_codec.c)
is a from-scratch version that drops the devicetree dependency (hardcoded
6-channel/44.1–48kHz/S16-S24-S32 capabilities) and self-registers its own
`platform_device`, so it needs no devicetree node — real hardware not
required on either side.

**The CPU-side DAI is still a real wall — sidestepped, not solved.** The
real machine driver is mainline `simple-audio-card` bound to
`rockchip,rk3588-i2s-tdm` — real RK3588 silicon QEMU's `virt` machine
doesn't emulate, confirmed unusable
([BUILDING.md](BUILDING.md#the-az04-codec-driver-exists-and-is-unusable-here)).
Instead of that, a small custom machine driver
([shims/rk3588/az04-audio/az04_card.c](../shims/rk3588/az04-audio/az04_card.c))
binds the reimplemented codec to ASoC's built-in dummy CPU DAI
(`snd_soc_dummy_dlc`, exported by `snd-soc-core`) — entirely in software,
no devicetree node, no real hardware on the CPU side either. Card name is
literally `"RMZ2"`, matching the real hardware's `simple-audio-card` node
name, in case anything string-matches on it (already tested once before
via a renamed HDA ALSA `id`, no effect — covered again here for
completeness).

This produced a genuinely new, fully-probing ALSA card
(`/proc/asound/cards` showing `1 [RMZ2]: RMZ2 - RMZ2`, real
`/dev/snd/pcmC1D0p`/`pcmC1D0c` nodes) — the first time this project has
gotten QEMU to present anything beyond the emulated `ich9-intel-hda`/
`hda-duplex` card to Engine.

**A real ASoC/ALSA bug, found and fixed.** Opening it (`aplay -D hw:1,0`)
failed immediately with `EINVAL`, before hw_params negotiation — same
symptom Engine itself hit, confirmed via `journalctl`:
`ALSADeviceEnumerator::notifyDeviceChanged` fires on the hotplug, then
`ALSADevice::open() Could not open device "hw:1" ""; reason: Invalid
argument`, then `scanDevices() Device initialization error` — on every
scan, and notably only ever for `hw:1` (the new card), never `hw:0` (the
working HDA card is never even tried). Root-caused with a kretprobe on
`snd_pcm_hw_constraint_mask()` (via `/sys/kernel/debug/tracing/
kprobe_events` — no rebuild needed) showing exactly one call, for
`ACCESS`, returning the failure, before format/channels/rate constraints
are ever reached. Cross-referencing against the actual kernel source
(`sound/soc/soc-utils.c`, matching Debian's `linux-image-arm64`
6.12.101+deb13-arm64 build the project's kernel already comes from —
[get_arm64_kernel.sh](../scripts/build_scripts/get_arm64_kernel.sh))
found the exact cause: `snd-soc-dummy`'s own `dummy_dma_open()` has a
guard —

```c
/* If there are other components associated with rtd, we shouldn't
 * override their hwparams */
for_each_rtd_components(rtd, i, component) {
    if (component->driver == &dummy_platform)
        return 0;
}
snd_soc_set_runtime_hwparams(substream, &dummy_dma_hardware);
```

— that, when `snd_soc_dummy_dlc` is itself used as the **platform**
(this project's case), finds *itself* in `rtd`'s component list and
returns before ever calling `snd_soc_set_runtime_hwparams()`. `hw.info`
stays `0` — no `SNDRV_PCM_INFO_INTERLEAVED` bit — so ALSA core's
`snd_pcm_hw_constraints_complete()` computes an empty `ACCESS` mask and
`open()` fails. `snd-soc-dummy` is built for DPCM/no-PCM backend roles,
not as a directly-openable platform in a plain link like this one. Fixed
in `az04_card.c` by registering a small custom platform component instead
(a real `.open` that calls `snd_soc_set_runtime_hwparams()` properly),
keeping `snd_soc_dummy_dlc` only for the CPU-DAI role, where it works
fine.

**Result: the ALSA-level open now succeeds — a different, deeper gate
remains.** With the fix loaded, the exact same hotplug event now shows
**no** `"Could not open device"` error at all — Engine's `EMain` thread
opens and holds both `pcmC1D0p` and `pcmC1D0c` open live (confirmed via
`/proc/<pid>/fd`), the first time in this whole investigation Engine has
successfully opened a non-real audio device end-to-end at the ALSA layer.
But in the exact same log burst, `airHost::updateAudioDeviceChanged`
still logs `Failed to fetch the audio device "" from the device
manager.` — unchanged. This conclusively separates two independent
mechanisms that were previously conflated: `ALSADeviceEnumerator`/
`ALSADevice` (`air.devicemanager.alsa`) does real, low-level ALSA
open/probe work and is now satisfied; `airHost::updateAudioDeviceChanged`
— the `dynamic_cast<airAudioDevice*>()` RTTI gate found via decompilation
— is a separate, still-unresolved check further downstream, indifferent
to whether a real device successfully opened underneath it.

Kernel module sources, Makefile, and build/deployment notes:
[shims/rk3588/az04-audio/](../shims/rk3588/az04-audio/). See
[BUILDING.md](BUILDING.md#reimplementing-az04-codec-as-a-loadable-kernel-module)
for the toolchain and live-deployment mechanics.

### Audio playback: found the exact `dynamic_cast` call site, live confirmation inconclusive

Follow-up to the above — pinning down what actually gates `airAudioDevice`
construction, now that a real device reaches `ALSADeviceEnumerator`
cleanly. Two threads, one that landed and one that didn't.

**The fix generalizes.** A second, independently-named card/codec pair
(`az04-codecdiag`/`az04-carddiag` — same source, `sed`-renamed, built and
loaded purely as a throwaway diagnostic, never committed) produces a
second ALSA card (`2 [RMZ2_1]`) that Engine also opens with zero errors —
confirms the `snd-soc-dummy` platform fix isn't something that happened
to work once by luck. One new, unexplained observation along the way:
`airHost::updateAudioDeviceChanged`'s `"Failed to fetch the audio device"`
warning fired for the *first* device-added event this boot (the original
`RMZ2` card) but did **not** re-fire for the second card's
`NOTIFY_AUDIO_DEVICE_ADDED` event a few minutes later in the same
process — suggesting that top-level callback is a one-shot/first-device
check, not something re-run per device. Not chased further yet.

**Found the real gate via static analysis.** Reopening the prior
session's saved Ghidra project (`Engine.bin`'s MD5 confirmed identical to
the currently-running `/proc/<pid>/exe`) and cross-referencing the
`airDevice`/`airAudioDevice` typeinfo addresses already recorded in
ENGINEOS.md's decompilation findings turned up `FUN_0181fdc0`, which
contains exactly the call this whole investigation has been chasing:

```c
plVar3 = (long *)__dynamic_cast(param_2,&PTR_vtable_02bf1cd8,&PTR_vtable_02c91850,0);
if (plVar3 != (long *)0x0) {
    (**(code **)(*plVar3 + 0x70))(plVar3,*(undefined4 *)(param_1 + 0x1dc));
    iVar2 = (**(code **)(*param_2 + 0x30))(param_2);
    if (iVar2 == 0) { /* ...store plVar3 into *(param_1 + 0x150) — playback slot */ }
    if (iVar2 == 1) { /* ...store plVar3 into *(param_1 + 0x158) — capture slot */ }
    uVar1 = FUN_017d8a00(param_1,param_2,param_3);
}
```

i.e., given a candidate `airDevice*`, this is the function that
`dynamic_cast`s it to `airAudioDevice*` and, on success, files it into a
playback or capture slot by asking the *original* object's own vtable
(offset `0x30`) which direction it is. The call itself is at file offset
`0181fe08` (`bl` to the external `__dynamic_cast` thunk at `00491718`),
return value in `x0` at the next instruction, `0181fe0c`.

**Live confirmation attempted, aborted for stability.** With Engine
already holding a real, open PCM device (first time this project has
gotten this far), the natural next step was catching this exact call
live via `gdbserver`+`lldb` (`gdb-remote`) to read `x0` at `0181fe0c` and
inspect the actual object's RTTI. Two real obstacles, in order:

1. **A real deadlock, not a debugger artifact.** `echo az04-card >
   .../unbind` on a platform device whose card Engine already has open
   hung the guest shell completely (uninterruptible — `Ctrl-C` did
   nothing), with no second tty and SSH disabled on this image, leaving a
   QEMU `system_reset` (guest reboot only, not a full relaunch — the
   `.ko` files survive since they were written to the real ext4 disk, not
   tmpfs) as the only recovery. Root cause not chased further, but the
   operational rule going forward is clear: **never unbind/rmmod a
   platform device whose ALSA card a live process has open** — trigger a
   fresh hotplug via a *new*, independent device instance instead (see
   the diagnostic-card approach above), never by tearing down one
   already in use.
2. **A persistent trap unrelated to the target breakpoint.** Even with
   `SIGCHLD` explicitly set to pass-without-stop
   (`process handle SIGCHLD -n false -p true -s false`), every `continue`
   landed on the exact same relative offset inside some shared library
   (`cmp x0,#0x0 / b.eq / b.lt / ret`, ASLR base shifting between
   attaches but the low bits identical every time) — repeatedly, instead
   of ever reaching `0181fe0c`. Not resolved; abandoned once it started
   looping rather than risk a second hang chasing it further.

**Where this leaves it**: the exact function, call site, and register
convention for the real gate are now known precisely (`FUN_0181fdc0`,
`0181fe08`/`0181fe0c` in `Engine.bin`, `x0` = cast result, `x19` =
original `airDevice*` — callee-saved copy of the incoming param, per the
function's own prologue). What's still open is *why* the cast fails for
an `ALSADevice`-wrapped card specifically — whether `ALSADevice` (the
class actually backing `hw:N` entries) is simply never constructed as
(or derived from) `airAudioDevice` regardless of which card it wraps, or
whether something about *this* card specifically still disqualifies it.
Continuing needs either a cleaner live-attach approach (breakpoint
directly at `0181fe0c` from process launch, before Engine's own startup
scan runs — sidesteps needing any later hotplug trigger at all) or
tracing `FUN_0181fdc0`'s caller(s) statically instead.

### Audio playback: `ALSADevice` and `airDevice` look structurally disconnected

Continuing the trace statically (`FUN_0181fdc0` has no direct callers —
Ghidra's call graph finds none, confirming it's reached only via a vtable
slot, not a `BL`). Cross-referencing `FUN_0181fdc0`'s own address found
it at slot `02bdbfa0` inside a large (20+ slot) multi-interface vtable
starting at `02bdbec0`, written by exactly two functions:
`FUN_0162c3e0` (constructor — reads `UserDataDir`, builds a `QDir` for
`DeviceConfiguration.json` via `QDir::absoluteFilePath`, exactly the file
ENGINEOS.md's Filesystem layout section already names as holding
audio/MIDI device selection) and `FUN_018154a0` (destructor — resets the
vptr to this same base before tearing down members, the standard Itanium
ABI pattern). This confirms `FUN_0181fdc0` is a virtual method of
`airHost` itself, not some unrelated helper class — the same class whose
`updateAudioDeviceChanged` logs the `""` empty-device warning.

Located `ALSADeviceEnumerator::scanDevices()` directly, via the literal
log strings it contains (`"virtual void
ALSADeviceEnumerator::scanDevices()"`, `"hw:%d"`) — it's `FUN_018222a0`,
already in the original typeinfo xref list from the first decompilation
pass. Its device-open sequence: `snd_card_next`/`snd_ctl_card_info`/
`snd_ctl_pcm_next_device` to enumerate `hw:N`, then **construct** a new
device object via `FUN_0180b6a8`, *then* call `ALSADevice::open()`
(`FUN_0181c1ac`, confirmed by its own `"Could not open device"` string —
matches every log line quoted above) on it. On success, it's added to an
internal linked list and, once per scan (gated on a "first device this
pass" flag), a virtual method at the new object's own vtable+`0x90` is
invoked on it directly — not on any `airHost`/observer object.

**Two structural findings, both pointing the same direction:**

1. `FUN_0180b6a8` (the `ALSADevice` constructor) writes four distinct
   vtable pointers (multiple bases/interfaces: `02be6f20`, `02be6fe8`,
   `02be7158`, referenced via `02be63c0`) — confirmed by dumping the
   *entire* decompiled function body and regex-matching every
   `PTR_FUN_*`/`PTR_vtable_*` reference in it. **None of them is
   `02bf1cd8`** — the `airDevice` vtable/typeinfo address this whole
   investigation has been tracking since the original decompilation.
   `ALSADevice` does not appear to write the `airDevice` vtable into
   itself anywhere in its own constructor.
2. `ALSADevice`'s primary vtable (`02be6f20`) has a **null pointer** at
   the slot immediately preceding it (`02be6f10`) where the Itanium ABI
   places a class's RTTI/`typeinfo` pointer (the slot right before it,
   `02be6f18`, holds `0x50` — a plausible small offset-to-top value,
   consistent with the surrounding layout). A null `typeinfo` for a
   vtable means the compiler emitted no RTTI for objects using it —
   `dynamic_cast`/`typeid` cannot introspect a *source* object through
   this vtable at all.

Followed the one confirmed post-open call (vtable+`0x90` on the freshly
constructed `ALSADevice`) to its actual target, `FUN_01803eec` — turned
out to be a plain logging/"mark as default" utility (logs device name,
`Default:`/`Playback:` flags via `QDebug`, sets one bool field), not an
observer dispatch or `airHost` registration call. So the actual point
where a device gets hip to `airHost`/`FUN_0181fdc0` at all is still
unlocated — it isn't the call this trace expected it to be.

**Net effect on the open question**: this is stronger and more specific
than "the cast fails" — `ALSADevice`, as actually constructed by
`scanDevices()`, shows no vtable-level connection to `airDevice` at all,
and its primary vtable can't even support being the source of a
`dynamic_cast` in the first place. That's consistent with a design where
ALSA cards are deliberately kept out of the `airDevice`/`airAudioDevice`
hierarchy entirely (matching the working theory that only some
hardcoded, non-ALSA path — the onboard `az04-codec` via a completely
different route, or nothing reachable under QEMU at all — was ever meant
to become `airAudioDevice`) rather than a fixable parameter/config
mismatch. Not yet proven, since the exact bridging point (if any) between
`ALSADevice` and whatever `param_2` actually is inside `FUN_0181fdc0`
hasn't been found — only ruled out as *not* being the constructor or the
one post-open virtual call this trace followed.

### Audio playback: found `airHost::updateAudioDeviceChanged` itself — the real, confirmed gate

`FUN_0181fdc0` (above) turned out to be a different, parallel mechanism.
Searching for the actual function behind the exact log message this
whole investigation started from meant finding every place in the binary
that calls through a vtable at offset `0xe0` — the slot `FUN_0181fdc0`
sits at — since it has no direct (`BL`) callers at all, only reachable
via a vtable. A full-binary instruction scan (5.77M instructions,
`ldr xN,[xM,#0xe0]` immediately followed by `blr xN` on the same
register — filters out the ~3200 raw offset-0xe0 hits down to 110 real
indirect-call sites) turned up `FUN_016170c0` twice — a function *already
flagged* in the very first Ghidra pass (back in the original
decompilation session) as one of the ~20 functions directly referencing
both the `airDevice` and `airAudioDevice` typeinfo addresses. That
convergence — found two completely different ways — was reason enough to
decompile it in full, and it is, unambiguously, `airHost::
updateAudioDeviceChanged(bool)` itself: its own decompiled body contains
the literal string `"void airHost::updateAudioDeviceChanged(bool)"` and
builds the exact log line already quoted throughout this document —
`"Failed to fetch the audio device "` + (device name) +
`" from the device manager."` — via `QMessageLogger::warning()`.

**The real gate, in the actual decompiled logic:** `updateAudioDeviceChanged`
builds a snapshot of `airHost`'s known-device list (from a field at
`this+0xa0`), searches it for an entry whose name matches the persisted
device preference (comparing against the `"Default Device"` sentinel
string documented earlier in this file), and — critically — once a
name-matching entry is found, does:

```c
plVar13 = (long *)__dynamic_cast(ppppppppuVar21[2], &PTR_vtable_02bf1cd8,
                                  &PTR_vtable_02c82780, 0);
if (plVar13 != (long *)0x0) {
    /* success path — uses plVar13 as the resolved airAudioDevice* */
    goto LAB_01617454;
}
break;  /* cast failed — falls through to the warning */
```

`&PTR_vtable_02bf1cd8`/`&PTR_vtable_02c82780` are the *exact same*
`airDevice`/`airAudioDevice` typeinfo addresses recorded at the very
start of this decompilation effort, now confirmed load-bearing in the
one function whose log output this entire investigation has been
chasing. If the cast fails, `plVar13` stays null, the loop `break`s, and
execution falls straight into the warning — matching the observed
behavior exactly, every single time, cast fix or no cast fix.

**Where this leaves the two-function picture**: `updateAudioDeviceChanged`
is the confirmed, authoritative gate — it's the one whose failure
produces the exact log line quoted throughout this document.
`FUN_0181fdc0` is a separate `airHost` virtual method doing a structurally
identical cast (same source/target types) but reached differently (no
direct callers, vtable-dispatched, stores results into
`this+0x150`/`this+0x158` rather than falling through to a warning) —
most likely a distinct registration/hotplug-time classification path
feeding the same `this+0xa0` device list that `updateAudioDeviceChanged`
later searches, rather than the same call.

**What's still unconfirmed**: whether `ppppppppuVar21[2]` — the list
entry actually fed into the winning `dynamic_cast` — is literally the
same `ALSADevice*` `ALSADeviceEnumerator::scanDevices()` constructs (in
which case the earlier finding that `ALSADevice`'s own vtable never
touches `airDevice`'s, and has null RTTI on its primary vtable, would be
the complete, proven explanation end-to-end), or some other object
inserted into `this+0xa0` by a step not yet traced. Confirming that means
finding what actually populates `this+0xa0` — the one remaining link in
an otherwise now fully-traced chain from `scanDevices()`'s `hw:N` open
through to the exact warning line.

Two more data points on that last link. First, the same
`__dynamic_cast(x, &PTR_vtable_02bf1cd8, &PTR_vtable_02c82780, 0)` idiom
— identical typeinfo pair, same source/target types — turns up in at
least two more `airHost` methods (`FUN_017d8020`, `FUN_017d8300`, both
already in the original xref list, both look like per-property
volume/mute handlers that need to resolve "the current default audio
device" on every call) — this cast is clearly *the* standard idiom this
class uses everywhere it needs an `airAudioDevice*`, not a one-off.

Second, a dead end worth recording so it isn't re-walked: the
`ALSADeviceEnumerator::scanDevices()` → `airHost` handoff was suspected
to be a direct call (the vtable+`0x90` call on the freshly-opened
`ALSADevice`, from the earlier finding) — but that's confirmed to be a
plain logging/self-info method (`FUN_01803eec`), not a registration
call, and it's dispatched through *`ALSADevice`'s own* vtable, not
`airHost`'s. The `"notifyDeviceChanged"` string has a second, unrelated
reference (`FUN_008ee4e0`) that turned out to be deep in generic
Qt/`QThread` event-loop machinery, not device-manager code. Both checked
and ruled out. The likely explanation: this handoff is a genuine Qt
signal/slot connection (`ALSADeviceEnumerator` emitting a signal,
`airHost` catching it in a slot via `QObject::connect`), which doesn't
show up as a direct call *or* a vtable dispatch at all in disassembly —
Qt's meta-object system resolves the connection at runtime through
`QMetaObject::activate()` using moc-generated indices, so finding it
needs a different technique (locating the actual `connect()` call sites
during `airHost`/`ALSADeviceEnumerator` construction, or the moc-generated
`qt_static_metacall` dispatch tables for both classes) rather than more
call-graph/vtable tracing.

Chased the Qt-signal theory one step further: found Qt6's actual
`QObject::connectImpl` (the function every modern function-pointer
`connect()` call compiles down to — confirmed via its external-symbol
name) and its internal PLT-equivalent thunk at `00492658`, with **417**
call sites across the whole binary — too many to review by hand, and
`airHost`'s own constructor (`FUN_0162c3e0`, fully decompiled, all
49164 characters of it) calls it **zero** times, and doesn't reference
`ALSADeviceEnumerator`/`scanDevices`/`ALSADevice::open` anywhere in its
body either — ruling out "airHost wires the connection in its own
constructor" outright, whether via Qt signals or a direct call.

One more concrete lead followed to its limit: `airHost`'s device-list
access is `*(long*)(this+0xa0)`, then `+0x70`, then `+0x10` — a
double-indirection suggesting `this+0xa0` isn't a list itself but a
*stored pointer to another object* (plausibly `ALSADeviceEnumerator`
itself, or a manager wrapping it) that owns the real list. If true, this
would mean there's no separate "registration" step at all — `airHost`
just holds a raw pointer set once, and reads `ALSADeviceEnumerator`'s
own list directly, which would make the earlier `ALSADevice`/`airDevice`
vtable-disconnection finding the complete, end-to-end explanation with
nothing left to bridge. But `ALSADeviceEnumerator`'s own list head (in
`scanDevices()`, fully decompiled) lives at `this+0x28` — not `+0x70`/
`+0x10` — so either this isn't the same object, or (more likely, given
C++ multiple inheritance routinely hands out `this`-pointers adjusted by
a fixed offset depending which base class a pointer is typed as) the
`+0xa0` pointer in `airHost` is `ALSADeviceEnumerator`'s address
*already adjusted* for some other base sub-object, making `+0x70`/`+0x10`
from *that* adjusted pointer land back on the same real field as `+0x28`
from the unadjusted one. Distinguishing those two possibilities by
further static reading alone is unreliable — it needs an actual memory
read of a live `airHost` instance's `this+0xa0` field to compare against
a live `ALSADeviceEnumerator`'s address directly, i.e. exactly the kind
of check the earlier live-`gdbserver` attempt was going for before
stability concerns cut it short. That remains the most promising next
step if this is picked up again: read `this+0xa0` from a live, paused
`airHost` instance (breakpointing at `updateAudioDeviceChanged`'s entry,
before it's had a chance to run, sidesteps the earlier hotplug-retrigger
problem entirely) and compare it directly against the known
`ALSADeviceEnumerator` instance address.

### Audio playback: live-attach session, and a real observer-effect finding

One more live-debugging round, this time solving the actual tooling
problem from before: `gdbserver` + remote `lldb` proved fundamentally
unreliable in this environment (a full session's worth of distinct
failure modes — a consistently recurring stray `SIGTRAP` on the very
first `continue` after any attach, later a `continue` that never
returned at all despite the target process visibly running fine, and a
real guest-level deadlock once from `echo ... > .../unbind` on a
platform device Engine already had a card open on, recovered only via
QEMU `system_reset`). Root cause never fully pinned down, but a
minimal, **native** `gdb` — built from source
(`../configure --without-python --without-guile --without-babeltrace
--without-debuginfod --disable-source-highlight --without-lzma
--disable-tui --without-expat`, in the same `debian:bookworm` container
already used for shim builds) with only 5 runtime deps instead of the
~56 Debian's packaged `gdb` needs (a prebuilt packaged `gdb` copied over
in full hit an immediate `Bus error` on `--version`, before ever
printing anything — never diagnosed, abandoned in favor of the minimal
build) — attached and ran locally on the guest, no network/remote-protocol
layer at all, and was dramatically more stable: real thread-lifecycle
events (`[New LWP ...]`, `[Detaching after vfork from child process
...]` matching `OfflineAnalyzer`/`crashpad_handler` spawning) instead of
the earlier unexplained stalls.

**The breakpoint still never hit — but this time with a real, confirmed
explanation.** Attaching `gdb` immediately after `systemctl restart
engine.service` (to catch `updateAudioDeviceChanged` from process
launch, avoiding any hotplug-retrigger dependency) meant ptrace-stopping
Engine within a fraction of a second of it starting. After over an
hour of wall-clock time — including triggering a genuinely fresh,
independent hotplug event partway through (a third diagnostic
`az04-codec`/`az04-card` pair, `...diag3`, built and loaded purely to
generate a new `NOTIFY_AUDIO_DEVICE_ADDED` without ever touching an
already-in-use card) — the breakpoint at `updateAudioDeviceChanged`'s
entry never fired once. Checking `journalctl` for the *entire* lifetime
of this specific process (not a recent time window — the full history)
settled it conclusively: `ALSADeviceEnumerator::notifyDeviceChanged`/
`NOTIFY_AUDIO_DEVICE_ADDED` **did** fire correctly in response to the
hotplug (confirming the notification pipeline itself works fine even
under ptrace) — but `airHost::updateAudioDeviceChanged`'s `"Failed to
fetch the audio device"` warning **never appeared even once** for this
process, neither at its own startup (unlike every previously-observed
process, including the very first ever capture of this exact log line,
which fired unconditionally within seconds of boot) nor after the
confirmed-successful hotplug notification.

The likely explanation: attaching a debugger *this* early — before
Engine has finished its own startup sequence — appears to suppress or
delay whatever normally triggers `updateAudioDeviceChanged` in the first
place, rather than just slowing it down. A real observer effect, not a
missed timing window (the wait was well over 100x the ~29-second
baseline observed in every non-debugged run this project has done). This
reframes the plan for any future attempt: attaching at process launch —
which seemed like the *safer* choice, since it avoids ever needing a
hotplug retrigger — is actually the wrong move. Attach only **after**
Engine has fully settled into its normal running state (confirmed via
`journalctl` already showing steady-state `DRMATOMIC` output, i.e. well
past its own startup sequence), then trigger a fresh hotplug event
exactly like the `...diag3` approach here, and only *then* arm the
breakpoint and continue.
