# DDJ-FLX10 jog screens
Shows Engine's decks on the Pioneer/AlphaTheta DDJ-FLX10's jog screens: waveform, artwork, BPM, key, tempo, time and position. The MONTERO mod in `shims/montero/mods/flx10` reads the deck state inside Engine and sends it out, and `flx10_bridge.py` turns it into the FLX10's screen protocol. The controls mapping is separate.

## Running it
- Install ffmpeg on the Pi, which the bridge uses to build waveforms from the track files: `sudo apt install ffmpeg`.
- Build `flx10.rcc` from `shims/montero/mods/flx10/flx10.qrc` with `rcc -binary` and put it in the rootfs at `/data/mods/flx10/`.
- Set the FLX10 lines in `pi.env`, from `pi.env.example`. They forward the controller to Engine with its mapping and preload MONTERO.
- Copy this directory to `~/qengine/flx10` and install the bridge as a service, which waits for the controller and picks it up again after a replug:

```sh
cd ~/qengine
sed "s|@QENGINE_DIR@|$PWD|" flx10/flx10-bridge.service | sudo tee /etc/systemd/system/flx10-bridge.service
sudo systemctl daemon-reload && sudo systemctl enable --now flx10-bridge.service
```

The bridge speaks through the kernel's own drivers and claims nothing, so midisurface can drive Engine from the same controller.

## Files
| File | What |
| --- | --- |
| `flx10_bridge.py` | The bridge |
| `flx10-bridge.service` | Runs the bridge at boot |
| `flx10_screen_test.py` | Drives the screens with fake decks, for protocol work without Engine |
| `flx10_art_test.py` | Sends one track's cover by hand, in the layouts that were tried |
| `flx10_probe.py` | Steps bytes of the deck state packet through values, to find what they drive |
| `rb_init_sequence.json` | rekordbox's start-up traffic to the controller, replayed to wake the screens |

## Limits
The firmware drops a waveform longer than about 24500 columns, which is 163 seconds at the rate it scrolls. Past that the bridge clips the end of the track, or with `--squeeze` fits the whole track into the limit. Whether a squeezed waveform stays in time with the music is not yet settled.

Beatgrid lines, hot cue markers and the bars countdown are not shown yet. The packets for them are either undecoded or unused.

## Credits
The screen protocol is the reverse-engineering work of Victor Pineda ([Veezuhz](https://github.com/Veezuhz/Mixxx_FLX10_Controller_Mapping)), who decoded the vendor unlock and the deck state packet, and Lougazi ([Loui1979](https://github.com/Loui1979/MIXXX-FLX10)), who captured rekordbox driving the controller and decoded its waveform packets. `rb_init_sequence.json` is from Loui1979's repository, which is public domain.
