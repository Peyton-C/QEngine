# Audio and MIDI
Engine discovers audio cards and MIDI clients through ALSA. QEngine adapts that discovery with `alsashim` and supplies a virtual control surface with `midisurface` on the EGLFS runtime. The legacy runtime has separate audio handling and no contemporary virtual surface.

## Audio
Engine filters ALSA cards by their reported name before inspecting PCM devices. Its per-product `AudioDevices` configuration can reject QEMU's `HDA Intel` card, leaving no audio device to select. Changing a kernel module's card `id` does not change the shortname Engine checks.

| Requirement | QEngine handling |
| --- | --- |
| Accepted card name | `alsashim` intercepts `snd_ctl_card_info_get_name()` |
| Compatible format and channels | Routes `hw:` PCM opens through ALSA's converting `plughw:` layer |
| Playback selected | Uses playback-only HDA, avoiding the capture device taking the default slot |
| Enough queued audio for QEMU | Enlarges the ring and thresholds without changing Engine's period size |
| Userspace MIDI client with a card number | Supplies a card number when ALSA reports none |

The name to report is the one the product's own audio driver registers. On RK3588 it is the product code. On RK3288 QEngine reads it from the firmware at build time with `detect_audio_card.sh`, because it is not always the product code: SC5000M (`JP08`) and SC6000 (`JP13`) come up as `JP07`, Prime GO+ (`JP11S`) as `JP11` and Mixstream Pro+ (`NH08S`) as `NH08`. Prime 4 and Prime 4+ (`JC11`, `JC11S`) play through an XMOS USB interface instead, named `PRIME 4 Audio`, while Prime 2 (`JC16`) has an ordinary card of its own name. The AZ05 products, Prime GO+ from 5.x, SC LIVE 2 and 4 and Mixstream Pro+ and Pro GO, keep their devicetrees in the update's boot partition rather than the rootfs, and `detect_audio_card.sh` reads them from the update image there.

QEngine gives both architectures QEMU's HDA card when the guest kernel has its driver, as Ubuntu's does, with `use-timer=false` so the card moves samples when the host asks for them. With QEMU's own timer, 96 kHz products such as the SC6000 lose a few samples several times a second. The card also has no emulated mixer, because Linux brings the codec's output amp up muted and Engine OS has nothing to raise it, and PipeWire, the default on Linux hosts, would carry that mute to the host. WirePlumber remembers it, so on a host that ran an instance before the mixer was removed, unmute QEMU's stream once with `wpctl`. PipeWire runs with a 20 ms audio timer; at QEMU's default 10 ms its stream clicks about 20 times a minute. Debian's armv7 kernel has no HDA driver, so on it QEngine falls back to virtio-sound, which stops for good once Engine falls behind and never starts with a ring under about 30 ms. Legacy Engine stays on virtio-sound on either kernel: Prime 4 and Prime 2 1.x open the card without `alsashim` and need a 32-bit format, which HDA does not offer, so on it Engine exits before drawing.

Use the instance launcher so the sound device and preload configuration stay together. A capture PCM can take Engine's default-device slot and leave playback unused; adding `hda-duplex` is not a way to add working inputs.

| `alsashim` setting | Effect |
| --- | --- |
| `ALSASHIM_AS` | Overrides the reported card name |
| `ALSASHIM_CARD` | Limits that override to one card index; Engine builds use 0, QEMU's card, so a USB interface keeps its own name |
| `ALSASHIM_BUFFER_SCALE` | Scales ring depth; default 8, which armv7 Engine builds also set, while the Pi and guest-linked legacy configuration use 1 |
| `ALSASHIM_XRUN_FREE_RUN` | Keeps playback running through an underrun instead of stopping it, which Engine never recovers from; off by default |
| `ALSASHIM_NO_CAPTURE` | Hides capture devices |
| `ALSASHIM_MAX_CHANNELS` | Limits advertised channels; the legacy SC/Prime GO configuration uses 4 |
| `ALSASHIM_METER` | Logs per-channel levels |

QEngine scales the buffer, period count and playback start/stop thresholds together. Raising buffer size alone can leave the stream underrunning. These settings adapt the output device; they do not supply hardware mixer inputs or motorized-platter timecode.

Each step of the scale adds one of Engine's rings of latency in QEMU. At 8, Prime 4 plays about 93 ms late and SC6000 about 75 ms, before QEMU and the host add their own. Scale 4 halves that and plays cleanly under KVM, but drops out briefly when a track loads during playback, and repeatedly under TCG. Below 4 the stream glitches. For low latency, use the [Pi chroot](../../scripts/pi/README.md), which runs Engine at its own buffer size.

The repository records audible end-to-end playback on arm64 RMZ2 5.0.4, and recorded playback under KVM on armv7 Prime 4, Prime 4+, Prime GO and SC6000 on 4.3.0 to 5.3.0, Prime GO+ on 4.3.4, and Prime 2, SC LIVE 2 and Mixstream Pro, Pro+ and Pro GO on 5.1.0b8 or 5.3.0. SC LIVE 4 (`JP21`) has no audio: Engine segfaults shortly after it accepts a sound card, on 5.1.0b8 and 5.3.0, with HDA or virtio-sound, and runs only with the card rejected. Under TCG on Apple Silicon, CoreAudio playback of Prime GO 5.3.0 cuts out about once a second whatever the buffer settings, so listen to armv7 under KVM. Legacy playback and MIDI binding remain unverified despite the guest-linked audio setup. See the [support matrix](../../README.md#engine-os-support-matrix) for other targets. The experimental `az04-codec` modules are not required by the normal playback path.

## MIDI discovery
The guest needs the USB audio/MIDI and ALSA sequencer kernel modules. QEngine's kernel build supplies them. Engine can enumerate generic sequencer clients, but enumeration does not establish a working control surface: it binds a control assignment only after a matching MIDI Device Inquiry reply.

The warning `The port isn't opened for Midi::Out::<name>` comes from closing a port that was never opened. It does not identify the audio-card rejection or a failed MIDI open.

`midisurface` answers with the selected product's identity. `alsashim` supplies the card number Engine requires for a userspace sequencer client. On supported later firmware, QEngine passes `-skipFirmwareUpdate` so Engine does not try to flash the virtual controller. Engine 1.x lacks that option, and the legacy launcher omits it.

## Controls
Engine maps surface messages through product-specific QML assignments. `midisurface` picks play, cue and load messages from its product table, since products can use different notes even on the same MIDI channels. Single-deck players accept commands without a deck name.

```sh
echo 'load left' > /run/midisurface.fifo
echo 'play left' > /run/midisurface.fifo
echo 'cue left' > /run/midisurface.fifo
```

Prime 4 (`JC11`) 5.3.0 ignores `load` while `play` works; load a track by touch there, double-tapping it and then tapping a deck. Run these in the guest with the surface service active. The Pi uses `/srv/engine/run/midisurface.fifo` from the host. See the [Pi guide](../../scripts/pi/README.md) for forwarding a real controller.

| Override | Effect |
| --- | --- |
| `MIDISURFACE_PRODUCT_CODE` | Selects the surface's product table entry |
| `MIDISURFACE_ID_RESPONSE` | Replaces its inquiry response with raw hex bytes |

Keep the surface identity consistent with Engine's selected product. A mismatched response can bind the wrong assignments or no surface at all. Settings may show a controller firmware version of zero because virtual-surface version bytes are unused when update checks are skipped.

## Motorized decks
RANE SYSTEM ONE starts in motorized mode and waits for platter timecode on the capture channels. Without that signal, play can wait while cue still previews audio. QEngine starts `midisurface --motor-off` for RMZ2 and sends the toggle when Engine binds the surface, including after Engine restarts.

The mode is not persisted, so it must be disabled at each launch. Automatic motor-off is restricted to `RMZ2`; SC5000M and SC6000M need their own mappings. Do not assume the RMZ2 Shift + Slip sequence applies to another product.

## Controller mappings
`controllermap` selects assignments for a connected USB controller and installs them where Engine loads the selected product's QML map. `midisurface --forward` relays its MIDI while answering Engine's identity inquiry. One mapping is active at a time, and the controller's LEDs and displays need their own protocol support. The [FLX10 guide](../../scripts/pi/flx10/README.md) covers that controller and its jog-screen bridge.
