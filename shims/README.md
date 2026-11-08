# Shims
Shims used to allow running unmodified Engine in QEMU

## alsashim
Allow for adding QEMU's fake audio card to Engine's audio card allowlist.

## cursorshim
Allows for a regular cursor to be displayed in Engine.

## drmatomic
Converts the ARGB8888 format frames from Qt to XRGB8888 for use with the virtio gpu.

## dtshim
Fakes various parts of the device tree expected by Engine and other InMusic OSes.

## midisurface
An emulated midi surface to control Engine.

## montero
Loads QML mods into Engine's UI, replacing its own files or adding new ones beside them. Built into Engine rootfs images, arm64 or armv7, with `new_instance.sh --montero`.

## pcmhold
Gives the kernel's loopback sound card the channel counts of the card it stands in for, by holding its partner device open. HeadRush uses it to stand in for HV01, 8 out and 2 in.

## qmlrotate
Rotates a Qt Quick window's content by `QMLROTATE` degrees. HeadRush uses it to turn Evil's UI, rotated for a sideways portrait panel, back to landscape.

## quitshim
Makes SIGTERM quit Engine cleanly, where Qt would exit on the spot. Used wherever Engine is stopped by a service manager: the VM builds, arm64 and armv7, and the Pi.

## teeshim
Bypass the TEE check added in Engine 5.1.0+.

## touchbridge
Converts the absolute values from QEMU's touch screen to a virtual touch screen.

## vnctouchbridge
Intercepts mouse input inbetween Qt's VNC server and the VNC client to convert it to a virtual touch screen.