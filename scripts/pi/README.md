# Running Engine on a Raspberry Pi 5
QEngine runs an instance's Engine OS rootfs in a chroot on a Pi 5, with no QEMU or other virtualization. Any arm64 Linux machine with a 4K-page kernel should work. Everything tried works except SoundSwitch: the display, touch from a USB mouse, USB and HDMI audio, a real MIDI controller through the virtual control surface, USB drives, and playback including stems.

## What has run
| Product | Arch | Firmware | Where | How |
| --- | --- | --- | --- | --- |
| Prime 4 G2 as `JP24` | arm64 | 5.1.1 | Pi 5 4GB, Raspberry Pi OS Lite 64-bit Trixie | display, sway |
| Prime 4 G2 as `JP24` | arm64 | 5.3.0 | Ubuntu 26.04 VM in UTM | GNOME, beside the MPCs |
| Prime 4 as `JC11` | armv7 | 5.3.0 | Pi 5 | display, sway |
| Prime 4 as `JC11` | armv7 | 4.3.0 | Pi 5 | display, sway |
| MPC Gen 2 as `ACVA3` | arm64 | 3.10.0 | Pi 5, UTM VM | sway on the Pi, GNOME in the VM |
| MPC Gen 1 as `ACVA` | armv7 | 3.10.0 | Pi 5 | sway, beside Gen 2 |
| MPC Sample | arm64 | 1.3.0 | UTM VM | GNOME |
| HeadRush VX5 | arm64 | 1.2.2 | UTM VM | GNOME |

## On the Pi, once
- Flash Raspberry Pi OS Lite (64-bit) with SSH enabled.
- Add `kernel=kernel8.img` to `/boot/firmware/config.txt`. A Pi 5 otherwise boots a 16K-page kernel, and the firmware is built for 4K pages. Keep the default `dtoverlay=vc4-kms-v3d` line.
- Optionally `sudo apt install systemd-container alsa-utils`. `aplay -l` and `aseqdump` are what you will reach for when something is silent.

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

An armv7 instance stages the same way and runs as 32-bit programs on the Pi's kernel. Its qtwayland builds in an emulated armv7 container, which takes about 45 minutes on an M-series Mac. For Classic Engine (4.3.4 and earlier, Qt 5), the stage carries the Mesa that replaces the Mali driver, rebuilt with the Pi's drivers, and with `--wayland` Qt 5's Wayland plugin from [build_qt5_qtwayland.sh](../build_scripts/build_qt5_qtwayland.sh). To keep an armv7 instance beside an arm64 one, stage it into a directory of its own, point `QENGINE_DIR` at it and set `ENGINE_ROOT` in its `pi.env`. An armv7 instance built before its eFuse fixtures were added to `write_fake_dt.sh` aborts at start-up, so rebuild it with `new_instance.sh --force`.

## On the Pi
```sh
cp qengine/pi.env.example qengine/pi.env   # optional; see Settings
sudo qengine/chroot-up.sh             # once per boot: mounts, /data, udev rules
sudo qengine/install_into_rootfs.sh   # once per rootfs, and after each re-stage
sudo qengine/chroot-engine.sh         # start, or restart, Engine
```

Engine takes over the display, so work over SSH. Logs are in `/srv/engine/run/`: `engine.log`, `touchbridge.log`, `midisurface.log`, `controllermap.log` and `edisksd.log`.

The virtual control surface takes the same commands as in a guest:
```sh
echo 'load left' | sudo tee /srv/engine/run/midisurface.fifo
echo 'play left' | sudo tee /srv/engine/run/midisurface.fifo
```

Nothing starts on boot unless you install [qengine.service](qengine.service), which runs `chroot-up.sh` and `chroot-engine.sh` with the settings in `pi.env`:
```sh
cd ~/qengine
sed "s|@QENGINE_DIR@|$PWD|" qengine.service | sudo tee /etc/systemd/system/qengine.service
sudo systemctl daemon-reload && sudo systemctl enable qengine.service
```

### Stopping
```sh
sudo qengine/chroot-down.sh           # or: sudo systemctl stop qengine
```
`chroot-down.sh` has Engine quit, stops what was started for it, then unmounts any USB drives and the chroot. The service runs it when stopped, so a shutdown or reboot does the same. Engine takes about seven seconds to quit. Pulling the power does none of this.

Restart Engine with `sudo systemctl restart qengine` when the service started it. Running `chroot-engine.sh` by hand works too, but that Engine belongs to your login session and stops with it at shutdown.

While Engine runs, the Pi's power button acts as a controller's does: a press opens Engine's Turn Off prompt, and Yes powers the Pi off. A second press while the prompt is up powers it off without an answer, as does a double press when Engine is hung. [powerkey.py](powerkey.py) does this and logs to `/srv/engine/run/powerkey.log`. With Engine stopped, a press powers the Pi off as usual.

### Listening from another machine
With no speakers on the Pi, Engine can play into ALSA's loopback card and another machine can pull the audio over SSH. `listen.sh` needs `sox` there, and the audio arrives a few hundred milliseconds late.
```sh
sudo AUDIO_CARD=Loopback qengine/chroot-engine.sh    # on the Pi
scripts/pi/listen.sh <user>@<pi>                     # where you want to hear it
```

### Headphones on a second card
Engine plays through one card, and a stereo card only gets the master. `CUE_CARD` names a second card for one more of Engine's outputs, so the master can go to the monitor and the headphone cue to a USB device:
```sh
sudo AUDIO_CARD=vc4hdmi1 CUE_CARD=H630 qengine/chroot-engine.sh
```

On a Prime 4 G2 the headphones are channels 3-4, the default, and the master is 1-2, repeated on 5-6 and 7-8. For another product, start once with `ALSASHIM_METER=1`, read the per-channel levels in `engine.log` and set `CUE_CHANNEL`. The second card runs 30 to 80 ms behind the first and, on its own clock, may glitch briefly every several minutes. The main output is not affected.

### Under sway
Engine OS ships Qt with eglfs only, so Engine takes over the display. Staged with `--wayland`, it can run as a window in [sway](https://swaywm.org/) instead, beside anything else, and the screen can be captured with `grim`. The stage then adds a libEGL with the Wayland platform from the same Mesa build, the libwayland libraries, and Qt's Wayland plugin, which [build_qtwayland.sh](../build_scripts/build_qtwayland.sh) builds for the rootfs's own Qt. eglfs keeps working with all of it installed.

```sh
scripts/pi/stage_for_pi.sh --name jp24-5.1.1 --wayland
```

On the Pi, `sudo apt install sway seatd` once. Then start sway with [sway.config](sway.config), and Engine in it:
```sh
mkdir -p ~/.config/sway && cp qengine/sway.config ~/.config/sway/config
# Over SSH, or anywhere without a seat of its own:
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland qengine/chroot-engine.sh
```

- Start sway with Engine stopped. An eglfs Engine holds the display, so sway comes up with no output and aborts when Engine's window appears.
- Set `WLR_RENDER_DRM_DEVICE`. Without it, sway hands its clients the display device, which cannot render, and Engine silently falls back to the CPU. `ls -l /proc/$(pgrep -f ^/usr/Engine/Engine)/fd | grep renderD128` shows Engine on the GPU.
- Engine has to be fullscreen, as sway.config makes it, and `QT_SCALE_FACTOR` has to be the window's height over 800, 1.8 at 1440, as with eglfs.
- The plugin, as built here, turns the left mouse button into touch (`QT_WAYLAND_MOUSE_AS_TOUCH=1`, set by `chroot-engine.sh`), since Engine ignores clicks. touchbridge and cursorshim are not used, and sway draws the pointer.
- A keyboard types into Engine. Engine OS's Qt lacks xkbcommon, so this build carries Qt's xkbcommon code and uses the rootfs's own libxkbcommon.
- `PLATFORM=wayland` can go in `pi.env`, but qengine.service does not start sway. Booting under sway is at the end of this section.

[sway-split.config](sway-split.config) puts Engine at its own 1280x800 in the top left of a 2560x1440 output, with one terminal beside it and one along the bottom:
```sh
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway -c qengine/sway-split.config > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland SCREEN="1280 800" QT_SCALE_FACTOR= qengine/chroot-engine.sh
```

Engine lays itself out for the screen, not its window, so in a window smaller than the output set `SCREEN` to the window's size, and the plugin reports that as the screen (`QT_WAYLAND_SCREEN_SIZE`). `QT_SCALE_FACTOR=` clears the one in `pi.env`.

[sway-rx3.config](sway-rx3.config) puts the XDJ-RX3 player from [Rx3-flx4](https://github.com/mutlisensor/Rx3-flx4) in place of the right-hand terminal. Set `RX3_DISPLAY=wayland` in its `rx3.conf`, and its `rx3-wl-present` shows the player as a window and takes clicks and touches from sway, in place of `rx3-fb-present` and `rx3-touch-bridge`:
```sh
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 LIBSEAT_BACKEND=seatd \
    XDG_RUNTIME_DIR=/run/user/$(id -u) setsid sway -c qengine/sway-rx3.config > /tmp/sway.log 2>&1 < /dev/null &
sudo PLATFORM=wayland SCREEN="1280 800" QT_SCALE_FACTOR= qengine/chroot-engine.sh
sudo systemctl start rx3
```

- With the FLX10's two USB ports split between Engine and the RX3, set `FLX10_PORT` in `pi.env` to Engine's port, so `flx10-bridge` drives the jog screens through it and leaves the other to the RX3. Each port's screens show that port's decks.
- The RX3's window can be any size. sway scales and letterboxes its 1280x800 picture, and clicks are mapped back.

To boot into this layout, with `qengine.service` and the RX3's `rx3.service` installed, add [qengine-sway.service](qengine-sway.service), which starts sway at boot, and the drop-ins [qengine-wayland.conf](qengine-wayland.conf) and [rx3-wayland.conf](rx3-wayland.conf), which start Engine and the RX3 in it:
```sh
cd ~/qengine
sed "s|@QENGINE_DIR@|$PWD|; s|@USER@|$(id -un)|" qengine-sway.service | sudo tee /etc/systemd/system/qengine-sway.service
sudo install -D -m644 qengine-wayland.conf /etc/systemd/system/qengine.service.d/wayland.conf
sudo install -D -m644 rx3-wayland.conf /etc/systemd/system/rx3.service.d/wayland.conf
sudo systemctl daemon-reload && sudo systemctl enable qengine-sway rx3
```

sway takes tty1, so the display shows no login prompt, and `chroot-down.sh` leaves tty1 alone while `qengine-sway` runs. To boot Engine alone again, `sudo rm -r /etc/systemd/system/qengine.service.d && sudo systemctl disable qengine-sway rx3`.

## MPC
[chroot-mpc.sh](chroot-mpc.sh) runs a Gen 1 or Gen 2 MPC, MPC Sample or HeadRush VX5 instance the same way, on any arm64 Linux machine with a 4K-page kernel, and tells them apart by the rootfs. Gen 1 is armv7 and needs a kernel and CPU that run 32-bit programs, as a Pi 5's do.
```sh
scripts/pi/stage_for_pi.sh --name mpc-gen2-3.10.0   # its two disks to build/pi-stage/mpc/, and kmsshim
rsync -a --sparse build/pi-stage/ <user>@<host>:qengine/
sudo qengine/chroot-mpc.sh          # mounts on first use, then starts or restarts MPC
sudo qengine/chroot-mpc.sh down     # stops MPC and unmounts everything
```

MPC needs only its rootfs and data disk. It draws without a GPU, and the instance already carries its shims. The host kernel needs `snd-dummy`, which becomes MPC's internal card as in a guest. Ubuntu's kernel has it, and the Pi's has not been checked. MPC's main output is copied to `AUDIO_CARD`.

- MPC restarts whenever it quits, as on the device, because quitting is how it switches between controller mode and standalone. Shutting down from MPC's own dialog stops it until the next `chroot-mpc.sh`, and the machine stays up.
- While MPC runs, the power button opens MPC's shutdown dialog.
- In a VM the QEMU tablet is the touchscreen. Elsewhere `touchbridge --mouse` turns a USB mouse into one, and no pointer is drawn.
- Logs are `mpc.log`, `touchbridge.log` and `mpcsurface.log` in `/srv/mpc/run/`. The panel takes commands on `/srv/mpc/run/mpcsurface.fifo`, as in a guest.
- Engine and MPC both take over the display, so run one at a time there. As windows, several run at once.
- Which of a Pi's two DRM devices MPC opens has not been tried.
- MPC Sample and VX5 need `snd-dummy` with the `qengine-ac50` model, which no distribution kernel has. Build it against the host kernel as [get_kernel.sh](../build_scripts/get_kernel.sh) does for guests and install it under `updates/`. Its default model is the stock card, so MPC keeps working. Set `MPC_DIR` to the Sample's disks. It has no touchscreen, and its panel is `mpcsurface` as in a guest.
- VX5 hears the WAV file in its `/data` as its input. `chroot-mpc.sh` starts `vx5surface` as its panel, driven through `/srv/mpc/run/vx5surface.fifo`, and writes the udev rule that links its devices to the paths it opens.
- To run several as windows, give each its own `MPC_ROOT` and `MPC_DIR`, and Engine can run beside them. `DUMMY_MODEL=qengine-ac50` puts all of them on one card, which MPC also accepts. Each app gets its own seat, so one window's input is not another's. Only the first app to open `AUDIO_CARD` gets it.

| Variable | Default | |
| --- | --- | --- |
| `MPC_DIR` | `~/qengine/mpc` | where `rootfs.img` and `data_disk.img` are |
| `MPC_ROOT` | `/srv/mpc` | where the rootfs is mounted |
| `AUDIO_CARD` | first USB audio card, else the first real card | as for Engine |
| `MPC_TB_ARGS` | none with a QEMU tablet, `--mouse` without | touchbridge options |
| `PLATFORM` | the display | `wayland` to run MPC as a window |

### MPC under Wayland
MPC draws its own frames and page-flips them through KMS, so with a compositor holding the display it has nowhere to draw. With `PLATFORM=wayland`, [kmsshim](../../shims/kmsshim/kmsshim.c) is preloaded into MPC as a fake KMS device, and [kmspresent](../../shims/kmsshim/kmspresent.c) on the host shows its frames as an ordinary window and passes the window's clicks and touches back as MPC's touchscreen. Log in to a desktop, or start sway as for Engine, then:
```sh
sudo PLATFORM=wayland qengine/chroot-mpc.sh
```

- The window can be any size. The app keeps its aspect ratio, and clicks are scaled back. A small screen opens at a whole multiple, the Sample's 320x240 at 960x720.
- kmspresent draws its frame with libdecor, since GNOME leaves that to each window. Install `libdecor-0-plugin-1-gtk` on the host, or the window has no frame. `chroot-mpc.sh` writes a hidden `.desktop` file for each app in `/usr/local/share/applications`, so a desktop shows it under its own name.
- MPC sees only the window's input. The host's keyboard, mice and power button stay with the compositor.
- kmspresent's touchscreen sits on its own udev seat, `seat-qengine`, which the compositor ignores, and kmsshim presents it to MPC as seat0. `chroot-mpc.sh` writes the rule.
- A desktop's PipeWire would open MPC's internal card too. `chroot-mpc.sh up` writes a WirePlumber rule that leaves it alone, so restart WirePlumber in a session that was already running. MPC still opens `AUDIO_CARD` directly, and gets it only while PipeWire has it suspended.
- Run as a window, `chroot-mpc.sh` leaves tty1 alone. Do not start a display manager there, as it conflicts with the login prompt and would end every desktop session.
- In the UTM VM, sway showed a black screen on UTM's display while drawing correctly, so use GNOME there. sway needs the user in the `video` group for seatd.

## Settings
`chroot-engine.sh` and `chroot-mpc.sh` read `~/qengine/pi.env`. [pi.env.example](pi.env.example) lists the common settings and [chroot-engine.sh](chroot-engine.sh) documents all of them. These are the ones you are most likely to need:

| Variable | Default | |
| --- | --- | --- |
| `AUDIO_CARD` | first USB audio card | ALSA id from `/proc/asound/cards` (`vc4hdmi1` is HDMI-A-2), a name in `/dev/snd/by-path` (the USB socket, for two cards with one id), or `Loopback` |
| `CUE_CARD` | none | a second card for the headphone output, with `CUE_CHANNEL` (default 3) picking the pair |
| `SCREEN` | `1280 800` | size to run at, which the monitor must offer. Other heights need `QT_SCALE_FACTOR`, see Known limits |
| `CONNECTOR` | first connected HDMI | such as `HDMI-A-2` |
| `PLATFORM` | `eglfs` | `wayland` to run in a compositor, see Under sway |
| `MIDI_FORWARD` | none | a real controller, by sequencer name |
| `MIDI_FORWARD_ARGS` | none | `midisurface` options that controller's mapping needs |

## Differences from a guest
| | On the Pi |
| --- | --- |
| Mesa | `stage_for_pi.sh` builds Mesa with `v3d` and `vc4` added (`build_virgl_mesa.sh --extra-drivers`). It keeps virgl, so the same library works in QEMU. |
| Display | A Pi has one DRM device that renders and another that owns the HDMI ports. `chroot-engine.sh` rewrites the product's `ScreenConfiguration.json` on each launch to name the right device, port and mode, and keeps the vendor file as `.stock`. |
| `drmatomic` | Not preloaded. It works around virtio-gpu, which the Pi does not have. |
| Touch | `touchbridge --mouse` turns any USB mouse into the touchscreen, and `cursorshim` draws the pointer, since a monitor shows none. |
| Audio | `alsashim` hides the card's inputs (`ALSASHIM_NO_CAPTURE`), giving SC LIVE 4 a silent one in their place since it crashes without an input, and uses Engine's own buffer size (`ALSASHIM_BUFFER_SCALE=1`). The Pi's HDMI audio only takes S/PDIF-framed samples, so `alsashim` opens it through ALSA's `hdmi:` device. For sound from the monitor, set `AUDIO_CARD` to the port's card, `vc4hdmi0` for HDMI-A-1 and `vc4hdmi1` for HDMI-A-2. |
| Services | There is no systemd in the chroot, so the two services Engine needs are started by hand: the rootfs's own D-Bus daemon, and `edisksd`, which mounts removable drives and tells Engine about them. |
| Quitting | A service manager's stop signal makes Qt exit on the spot, which nothing on a real device ever sends. `quitshim` turns it into Engine's own clean exit, and stands in for the firmware's splash service, without which Engine aborts on its way out. |
| udev | The chroot uses the host's udev database. `chroot-up.sh` installs two rules on the host so that `edisksd` leaves the Pi's own SD card alone and skips non-media partitions, as on real hardware. |

## Known limits
- Engine lays out for an 800-pixel-high screen and hangs at start-up at any other height. To run at 1920x1080 or 2560x1440, have Qt scale the interface so it still sees 800: `SCREEN="1920 1080"` with `QT_SCALE_FACTOR=1.35`, or 2560x1440 with 1.8.
- SoundSwitch does not work. It failed in a chroot on a VM and has not been looked into.
- Bluetooth and network management are not running, and Engine logs warnings about both.
- The stock launch scripts write to Rockchip-specific sysfs paths and fail harmlessly, as they do under QEMU.
