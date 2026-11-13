# Shims
Shims used to allow running unmodified Engine in QEMU

## alsashim
Allows Engine to use emulated sound cards. The SC media players and Prime GO on 1.5.2 and later use a guest-linked build for PCM conversion and four-channel playback-only audio. MPC and HeadRush VX5 use it only to copy what they play on `snd-dummy` onto QEMU's sound card, and VX5 also to hear a WAV file in place of its input.

## cursorshim
Allows for a regular cursor to be displayed in Engine.

## drmatomic
Converts the ARGB8888 format frames from Qt to XRGB8888 for use with the virtio gpu.

## dtshim
Fakes various parts of the device tree expected by Engine and other InMusic OSes.

## kmsshim
A fake KMS device for apps that drive the display themselves, preloaded into MPC so it can run as a window under a Wayland compositor. `kmspresent`, beside it, runs on the host: it shows the app's frames as a window and gives the app a touchscreen from the window's input. Used by `chroot-mpc.sh` with `PLATFORM=wayland`.

## midisurface
An emulated midi surface to control Engine.

## mpcsurface
An emulated control surface for MPC, Force and MPC Sample: pads, buttons, the dial, Q-Links and the Sample's knobs. Preloaded into the app, which finds it as its panel's USB MIDI card, or on MPC Sample as its serial "Control Surface" card, and driven through `/run/mpcsurface.fifo`. See [mpcsurface](mpcsurface/README.md) for commands.

## montero
Loads QML mods into Engine's UI, replacing its own files or adding new ones beside them. Built into Engine rootfs images, arm64 or armv7, with `new_instance.sh --montero`.

## pcmhold
Gives the kernel's loopback sound card the channel counts of the card it stands in for, by holding its partner device open. HeadRush Prime uses 8 out and 2 in.

## qmlrotate
Rotates a Qt Quick window's content by `QMLROTATE` degrees. HeadRush and early SC5000 builds use it to turn their UI, rotated for a sideways portrait panel, back to landscape.

## qt55vnc
Serves the native Qt 5.5 offscreen display over VNC and supplies raster placeholders for OpenGL textures on SC5000 1.0.x. See [qt55vnc](rk3288/qt55vnc/README.md).

## quitshim
Makes SIGTERM quit Engine cleanly, where Qt would exit on the spot. Used wherever Engine is stopped by a service manager: the VM builds, arm64 and armv7, and the Pi.

## teeshim
Bypass the TEE check added in Engine 5.1.0+.

## touchbridge
Converts the absolute values from QEMU's touch screen to a virtual touch screen.

## vnctouchbridge
Intercepts mouse input inbetween Qt's VNC server and the VNC client to convert it to a virtual touch screen.

## vx5surface
An emulated panel for HeadRush VX5: its encoders, switches, phantom-power key and volume knob as input devices at the paths the app opens, and its USB MIDI port as a sequencer client named `f_midi`. Driven through `/run/vx5surface.fifo`.
