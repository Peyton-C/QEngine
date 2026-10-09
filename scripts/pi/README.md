# Running Engine on a Raspberry Pi 5
Runs an instance's Engine OS rootfs in a chroot on a RPI, with no QEMU or other virtualization. Could be used with other arm64 linux devices with a 4k kernel.

Tested with a Raspberry Pi 5 4GB on Raspberry Pi OS Lite 64-bit Trixie, with a spoofed Denon Prime 4 G2 (JP24) on Engine OS 5.1.1. Everything tested, including fake touch with a usb mouse, display, audio (USB and HDMI), a real midi controller (via virtual control surface), external USB media, and playback (including pre-rendered stems), works, with the only exception being SoundSwitch.

## On the Pi, once
- Flash Raspberry Pi OS Lite (64-bit) with SSH enabled.
- Add `kernel=kernel8.img` to `/boot/firmware/config.txt`. A Pi 5 otherwise boots a 16K-page kernel, and the firmware is built for 4K pages. The default `dtoverlay=vc4-kms-v3d` line has to stay.
- `sudo apt install systemd-container alsa-utils` is not required, but `aplay -l` and `aseqdump` are what you will reach for when something is silent.

## Build machine to Pi
```sh
# An arm64 Engine instance, as for QEMU. It must not be running.
scripts/build_scripts/new_instance.sh --name jp24-5.1.1 --device engine \
    --product-code JP24 --firmware /path/to/PRIME4G2-5.1.1-Update.img

# Gather the rootfs, a Mesa with the Pi's GPU drivers, freshly built shims,
# controllermap and these scripts into build/pi-stage/.
scripts/pi/stage_for_pi.sh --name jp24-5.1.1

# Copy across, keeping the 4GB image sparse.
rsync -a --sparse build/pi-stage/ <user>@<pi>:qengine/
```

## On the Pi

```sh
cp qengine/pi.env.example qengine/pi.env   # optional; see below
sudo qengine/chroot-up.sh             # once per boot: mounts, /data, udev rules
sudo qengine/install_into_rootfs.sh   # once per rootfs, and after each re-stage
sudo qengine/chroot-engine.sh         # start, or restart, Engine
```

Engine takes over the display. Work over SSH. Logs are in `/srv/engine/run/`: `engine.log`, `touchbridge.log`, `midisurface.log`, `controllermap.log`,`edisksd.log`.

The virtual control surface takes the same commands as in a guest:
```sh
echo 'load left' | sudo tee /srv/engine/run/midisurface.fifo
echo 'play left' | sudo tee /srv/engine/run/midisurface.fifo
```

Nothing starts on boot unless you ask for it:

```sh
cd ~/qengine
sed "s|@QENGINE_DIR@|$PWD|" qengine.service | sudo tee /etc/systemd/system/qengine.service
sudo systemctl daemon-reload && sudo systemctl enable qengine.service
```

[qengine.service](qengine.service) runs `chroot-up.sh` and `chroot-engine.sh` at boot with the settings in `pi.env`.

### Listening from another machine
With no speakers on the Pi, Engine can play into ALSA's loopback card and another machine can pull the audio over SSH:

```sh
sudo AUDIO_CARD=Loopback qengine/chroot-engine.sh    # on the Pi
scripts/pi/listen.sh <user>@<pi>                     # where you want to hear it
```

`listen.sh` needs `sox` on the listening machine. Expect a few hundred milliseconds of delay.

### Headphones on a second card
Engine plays through one sound card, and a stereo card only gets the master. `CUE_CARD` names a second card for one more of Engine's outputs, so the master can go to the monitor and the headphone cue to a USB device:

```sh
sudo AUDIO_CARD=vc4hdmi1 CUE_CARD=H630 qengine/chroot-engine.sh
```

On a Prime 4 G2 the headphones are channels 3-4, which is the default; the master is 1-2 and is repeated on 5-6 and 7-8. For another product, start once with `ALSASHIM_METER=1` and read the per-channel levels in `engine.log`, then set `CUE_CHANNEL`. The second card runs 30-80ms behind the first and, having its own clock, may glitch briefly every several minutes; the main output is not affected.

## Settings

Both scripts read `~/qengine/pi.env`; [pi.env.example](pi.env.example) lists the common ones and [chroot-engine.sh](chroot-engine.sh) documents all of them. The ones you are most likely to need:

| Variable | Default | |
| --- | --- | --- |
| `AUDIO_CARD` | first USB audio card | ALSA id from `/proc/asound/cards` (`vc4hdmi1` is HDMI-A-2), or `Loopback` |
| `CUE_CARD` | none | a second card for the headphone output; `CUE_CHANNEL` (default 3) picks the pair |
| `SCREEN` | `1280 800` | size to run at; the monitor must offer it. Other heights need `QT_SCALE_FACTOR`, see Known limits |
| `CONNECTOR` | first connected HDMI | e.g. `HDMI-A-2` |
| `MIDI_FORWARD` | none | a real controller, by sequencer name |
| `MIDI_FORWARD_ARGS` | none | `midisurface` options that controller's mapping needs |

## What differs from a guest, and why
- **Mesa.** The guest's library only has virgl. `stage_for_pi.sh` builds one
  with `v3d` and `vc4` added (`build_virgl_mesa.sh --extra-drivers`); it still
  has virgl, so the same file works in QEMU.
- **Display device.** A Pi has two DRM devices, one that renders and one that
  owns the HDMI ports. `chroot-engine.sh` rewrites the product's
  `ScreenConfiguration.json` on each launch to name the right device, the
  connected port and the mode. The vendor file is kept as `.stock`.
- **No `drmatomic`.** It works around virtio-gpu and is not preloaded here.
- **Touch.** `touchbridge --mouse` turns any USB mouse into the touchscreen, and
  `cursorshim` draws the pointer, since a monitor shows none.
- **Audio.** `alsashim` hides the card's inputs (`ALSASHIM_NO_CAPTURE`) and uses
  Engine's own buffer size (`ALSASHIM_BUFFER_SCALE=1`). The Pi's HDMI sound
  device only takes S/PDIF-framed samples, which the usual conversion layer
  cannot produce, so `alsashim` opens it through ALSA's `hdmi:` device instead.
  For sound from the monitor, set `AUDIO_CARD` to the port's card: `vc4hdmi0`
  for HDMI-A-1, `vc4hdmi1` for HDMI-A-2.
- **Services.** There is no systemd in the chroot, so the two Engine needs are
  started by hand: the rootfs's own D-Bus daemon, and `edisksd`, which mounts
  removable drives and tells Engine about them.
- **udev.** The chroot uses the host's udev database. `chroot-up.sh` installs two
  rules on the host so that `edisksd` leaves the Pi's own SD card alone and
  skips non-media partitions, as it does on real hardware.

## Known limits
- Engine's interface is laid out for an 800-pixel-high screen and hangs at start-up at any other height (1920x1080, 2560x1440): a height binding in its QML never settles, and the main thread spins in the layout pass. To run at a larger size, have Qt scale the interface so that it still sees 800: `SCREEN="1920 1080"` with `QT_SCALE_FACTOR=1.35` (1080 / 800). 2560x1440 with 1.8 works the same way.
- Bluetooth and network management are not running, and Engine logs warnings about both.
- The stock launch scripts write to Rockchip-specific sysfs paths and fail harmlessly, as they do under QEMU.
- SoundSwitch did not work when tried in a chroot on a VM, not looked into yet.
