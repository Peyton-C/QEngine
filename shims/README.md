# Shims
Shims used to allow running unmodified Engine in QEMU

## alsashim
Allow for adding QEMU's fake audio card to Engine's audio card allowlist.

## drmatomic
Converts the ARGB8888 format frames from Qt to XRGB8888 for use with the virtio gpu.

## dtshim
Fakes various parts of the device tree expected by Engine and other InMusic OSes.

## midisurface
An emulated midi surface to control Engine.

## teeshim
Bypass the TEE check added in Engine 5.1.0+.

## touchbridge
Converts the absolute values from QEMU's touch screen to a virtual touch screen.

## vnctouchbridge
Intercepts mouse input inbetween Qt's VNC server and the VNC client to convert it to a virtual touch screen.