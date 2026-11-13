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

Use the instance launcher so the sound device and preload configuration stay together. A capture PCM can take Engine's default-device slot and leave playback unused; adding `hda-duplex` is not a way to add working inputs.

| `alsashim` setting | Effect |
| --- | --- |
| `ALSASHIM_AS` | Overrides the reported card name |
| `ALSASHIM_BUFFER_SCALE` | Scales ring depth; default 8, while the Pi and guest-linked legacy configuration use 1 |
| `ALSASHIM_NO_CAPTURE` | Hides capture devices |
| `ALSASHIM_MAX_CHANNELS` | Limits advertised channels; the legacy SC/Prime GO configuration uses 4 |
| `ALSASHIM_METER` | Logs per-channel levels |

QEngine scales the buffer, period count and playback start/stop thresholds together. Raising buffer size alone can leave the stream underrunning. These settings adapt the output device; they do not supply hardware mixer inputs or motorized-platter timecode.

The repository records audible end-to-end playback on arm64 RMZ2 5.0.4. Legacy playback and MIDI binding remain unverified despite the guest-linked audio setup. See the [support matrix](../../README.md#engine-os-support-matrix) for other targets. The experimental `az04-codec` modules are not required by the normal playback path.

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

Run these in the guest with the surface service active. The Pi uses `/srv/engine/run/midisurface.fifo` from the host. See the [Pi guide](../../scripts/pi/README.md) for forwarding a real controller.

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
