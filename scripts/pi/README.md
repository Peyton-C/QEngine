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

### Stopping
```sh
sudo qengine/chroot-down.sh           # or: sudo systemctl stop qengine
```
The Pi's pwoer button works similarly to the power button on real controllers, with the exception being that a double press will shut the pi without confirmation in Engine still. [powerkey.py](powerkey.py) is what connects the native behavior of Engine and the Pi together, and it only runs alongside Engine: with Engine stopped, a single press powers the Pi off as usual. Its log is `/srv/engine/run/powerkey.log`.

`chroot-down.sh` has Engine quit, stops what was started for it, unmounts any USB drives and then the chroot itself. The service runs it when it is stopped, so shutting the Pi down or rebooting it does the same; Engine takes about seven seconds to quit. Pulling the power does none of this.

Restart Engine with `sudo systemctl restart qengine` when it was started by the service. `chroot-engine.sh` run by hand works too, but the Engine it starts belongs to your login session and not to the service, and at shutdown is stopped along with that session.

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

### Under sway (Wayland)
Engine OS ships Qt with eglfs only, so by default Engine takes over the display. With the Wayland pieces added back in, it can run as a window in [sway](https://swaywm.org/) instead, next to anything else, and the screen can be captured (`grim`).

Stage with `--wayland`. That adds a libEGL with the Wayland platform (from the same Mesa build as the libgallium), the libwayland libraries, and Qt's Wayland plugin, which [build_qtwayland.sh](../build_scripts/build_qtwayland.sh) builds for the rootfs's own Qt and checks against it. eglfs keeps working with all of it installed.

```sh
scripts/pi/stage_for_pi.sh --name jp24-5.1.1 --wayland
```

On the Pi, once: `sudo apt install sway seatd`. Then start sway with [sway.config](sway.config), and Engine in it:

```sh
mkdir -p ~/.config/sway && cp qengine/sway.config ~/.config/sway/config
# Over SSH, or anywhere without a seat of its own:
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland qengine/chroot-engine.sh
```

- Start sway with Engine stopped. An eglfs Engine holds the display, sway then comes up with no output, and aborts as soon as Engine's window appears.
- `WLR_RENDER_DRM_DEVICE` is required. sway otherwise renders through the display device, which has no render node, and tells its clients to use it; Engine's Mesa cannot, and renders on the CPU without a word in any log. With it, sway and Engine both render on the v3d render node. `ls -l /proc/$(pgrep -f ^/usr/Engine/Engine)/fd | grep renderD128` shows Engine on the GPU.
- Engine has to be fullscreen, as sway.config makes it: the window's height over 800 is what `QT_SCALE_FACTOR` must be (1440 / 800 = 1.8), as with eglfs.
- Engine ignores mouse clicks. The Wayland plugin, as built here, turns the left button into touch (`QT_WAYLAND_MOUSE_AS_TOUCH=1`, which chroot-engine.sh sets), so touchbridge and cursorshim are not used; sway draws the pointer.
- A keyboard types into Engine through sway (tried in search). Engine OS's Qt is built without xkbcommon, which the Wayland plugin needs to turn keys into characters, so this build carries Qt's xkbcommon code itself and uses the rootfs's own libxkbcommon.
- `PLATFORM=wayland` can go in `pi.env`, but qengine.service does not start sway by itself; for booting under sway, see the end of this section.

[sway-split.config](sway-split.config) instead puts Engine at its own 1280x800 in the top left of a 2560x1440 output, with a terminal beside it and one along the bottom:

```sh
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway -c qengine/sway-split.config > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland SCREEN="1280 800" QT_SCALE_FACTOR= qengine/chroot-engine.sh
```

Engine lays itself out for the screen rather than its window, so in a window smaller than the output `SCREEN` has to be the window's size: the Wayland plugin then reports the screen as that size (`QT_WAYLAND_SCREEN_SIZE`, another option of this build of it). `QT_SCALE_FACTOR=` clears the one in `pi.env`.

[sway-rx3.config](sway-rx3.config) puts the XDJ-RX3 player from [Rx3-flx4](https://github.com/mutlisensor/Rx3-flx4) where the right-hand terminal was, with `RX3_DISPLAY=wayland` in its `rx3.conf`. Its `rx3-wl-present` then shows the player's picture as a window and takes clicks and touches from sway, in place of `rx3-fb-present` and `rx3-touch-bridge`:

```sh
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway -c qengine/sway-rx3.config > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland SCREEN="1280 800" QT_SCALE_FACTOR= qengine/chroot-engine.sh
sudo systemctl start rx3
```

- With the FLX10's two USB ports on Engine and the RX3, set `FLX10_PORT` in `pi.env` to Engine's port, so `flx10-bridge` drives the jog screens through it and leaves the other to the RX3. Each port's screens show the decks that port has.
- The RX3's window can be any size. Its 1280x800 picture is scaled by sway and letterboxed, and clicks are mapped back through the same scaling.

To boot into this layout, with `qengine.service` and the RX3's `rx3.service` already installed: [qengine-sway.service](qengine-sway.service) starts sway at boot, and two drop-ins put Engine ([qengine-wayland.conf](qengine-wayland.conf)) and the RX3 ([rx3-wayland.conf](rx3-wayland.conf)) in it once it is up.

```sh
cd ~/qengine
sed "s|@QENGINE_DIR@|$PWD|; s|@USER@|$(id -un)|" qengine-sway.service | sudo tee /etc/systemd/system/qengine-sway.service
sudo install -D -m644 qengine-wayland.conf /etc/systemd/system/qengine.service.d/wayland.conf
sudo install -D -m644 rx3-wayland.conf /etc/systemd/system/rx3.service.d/wayland.conf
sudo systemctl daemon-reload && sudo systemctl enable qengine-sway rx3
```

- sway takes tty1, so there is no login prompt on the display; `chroot-down.sh` leaves tty1 alone while `qengine-sway` is running.
- To boot Engine alone again: `sudo rm -r /etc/systemd/system/qengine.service.d && sudo systemctl disable qengine-sway rx3`.

## MPC
[chroot-mpc.sh](chroot-mpc.sh) runs a Gen 2 MPC instance the same way, on any arm64 Linux machine with a 4K-page kernel. It was tried with MPC 3.10.0 as `ACVA3` in an Ubuntu 26.04 VM under UTM, and not yet on a Pi. Gen 1 images are armv7 and it does not run them.

```sh
scripts/pi/stage_for_pi.sh --name mpc-gen2-3.10.0   # its two disks to build/pi-stage/mpc/, and kmsshim
rsync -a --sparse build/pi-stage/ <user>@<host>:qengine/
sudo qengine/chroot-mpc.sh          # mounts on first use, then starts or restarts MPC
sudo qengine/chroot-mpc.sh down     # stops MPC and unmounts everything
```

MPC needs nothing staged beyond its rootfs and data disk to run on the display. It draws without a GPU, and the instance already carries its shims. The host kernel has to have `snd-dummy`, which becomes MPC's internal card, as in a guest; Ubuntu's does, and the Pi's has not been checked. MPC's main output is copied to `AUDIO_CARD`.

- MPC is restarted whenever it quits, as on the device, because it quits to switch between controller mode and standalone. Shutting down from MPC's own dialog stops it until the next `chroot-mpc.sh`; the machine stays up.
- While MPC runs, the power button opens its shutdown dialog instead of turning the machine off.
- In a VM, the QEMU tablet is the touchscreen. Elsewhere `touchbridge --mouse` turns a USB mouse into one, and no pointer is drawn.
- Logs are `mpc.log`, `touchbridge.log` and `mpcsurface.log` in `/srv/mpc/run/`. The panel takes commands on `/srv/mpc/run/mpcsurface.fifo`, as in a guest.
- Engine and MPC both take over the display, so run one at a time.
- A Pi has two DRM devices, and which one MPC opens has not been tried.

| Variable | Default | |
| --- | --- | --- |
| `MPC_DIR` | `~/qengine/mpc` | where `rootfs.img` and `data_disk.img` are |
| `MPC_ROOT` | `/srv/mpc` | where the rootfs is mounted |
| `AUDIO_CARD` | first USB audio card, else the first real card | as for Engine |
| `MPC_TB_ARGS` | none with a QEMU tablet, `--mouse` without | touchbridge options |
| `PLATFORM` | the display | `wayland` to run MPC as a window, as below |

### MPC under Wayland
MPC draws its own frames and page-flips them through KMS, so a compositor holding the display would leave it nowhere to draw. With `PLATFORM=wayland`, [kmsshim](../../shims/kmsshim/kmsshim.c) is preloaded into MPC as its display: a fake KMS device whose frames [kmspresent](../../shims/kmsshim/kmspresent.c), on the host, shows as an ordinary window. kmspresent is also MPC's touchscreen, from the window's clicks and touches. Log in to a desktop, or start sway as for Engine, then:

```sh
sudo PLATFORM=wayland qengine/chroot-mpc.sh
```

- The window can be any size. MPC keeps its 1280x800 aspect ratio inside it, and clicks are scaled back.
- MPC sees no input but the window's. The host's keyboard and mice stay with the compositor, and its power button is not MPC's.
- kmspresent's touchscreen is on a udev seat of its own, `seat-qengine`, which the compositor ignores and kmsshim shows MPC as its seat0. `chroot-mpc.sh` writes the rule.
- A desktop's PipeWire would open MPC's internal card too. `chroot-mpc.sh up` writes a WirePlumber rule that leaves it alone; restart WirePlumber in a session that was already running. MPC still opens `AUDIO_CARD` directly, and gets it only while PipeWire has it suspended.
- Run as a window, `chroot-mpc.sh` leaves tty1 alone. A display manager conflicts with the login prompt there, so starting it would end every desktop session.
- Tried in GNOME 50 on Ubuntu 26.04 in UTM, with sound. sway in the same VM showed a black screen on UTM's display while drawing correctly, so it was not tried further; it needs the user in the `video` group for seatd. Not tried on a Pi.

## Settings

Both scripts read `~/qengine/pi.env`; [pi.env.example](pi.env.example) lists the common ones and [chroot-engine.sh](chroot-engine.sh) documents all of them. The ones you are most likely to need:

| Variable | Default | |
| --- | --- | --- |
| `AUDIO_CARD` | first USB audio card | ALSA id from `/proc/asound/cards` (`vc4hdmi1` is HDMI-A-2), a name in `/dev/snd/by-path` (the USB socket, for two cards with one id), or `Loopback` |
| `CUE_CARD` | none | a second card for the headphone output; `CUE_CHANNEL` (default 3) picks the pair |
| `SCREEN` | `1280 800` | size to run at; the monitor must offer it. Other heights need `QT_SCALE_FACTOR`, see Known limits |
| `CONNECTOR` | first connected HDMI | e.g. `HDMI-A-2` |
| `PLATFORM` | `eglfs` | `wayland` to run in a compositor; see Under sway |
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
- **Quitting.** Nothing on the real system ever stops Engine from outside, andb the signal a service manager stops it with makes Qt exit on the spot. `quitshim` turns that signal into Engine's own clean exit, and stands in for the firmware's splash service, which Engine aborts without on its way out.
- **udev.** The chroot uses the host's udev database. `chroot-up.sh` installs two
  rules on the host so that `edisksd` leaves the Pi's own SD card alone and
  skips non-media partitions, as it does on real hardware.

## Known limits
- Engine's interface is laid out for an 800-pixel-high screen and hangs at start-up at any other height (1920x1080, 2560x1440): a height binding in its QML never settles, and the main thread spins in the layout pass. To run at a larger size, have Qt scale the interface so that it still sees 800: `SCREEN="1920 1080"` with `QT_SCALE_FACTOR=1.35` (1080 / 800). 2560x1440 with 1.8 works the same way.
- Bluetooth and network management are not running, and Engine logs warnings about both.
- The stock launch scripts write to Rockchip-specific sysfs paths and fail harmlessly, as they do under QEMU.
- SoundSwitch did not work when tried in a chroot on a VM, not looked into yet.
