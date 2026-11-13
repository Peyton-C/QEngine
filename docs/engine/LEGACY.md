# Legacy Engine OS
Legacy Engine OS covers armv7 firmware with glibc older than 2.34, including Engine 1.x and early 2.x. QEngine builds its shims against the guest's own libraries and uses software rendering. It keeps the vendor Mali libraries rather than installing the newer Mesa stack.

## Runtime
| Firmware runtime | QEngine handling |
| --- | --- |
| SC5000 1.0.x, Qt 5.5.1 | Native Qt, a matching raster scenegraph plugin and `qt55vnc` serving the offscreen display |
| Qt 5.9 and 5.12 | Qt VNC, `vnctouchbridge` and `qmlrotate` where landscape correction is needed |
| Qt 5.15.2 with older glibc | Qt VNC and `vnctouchbridge`; no rotation shim needed |
| SC media players and Prime GO, Qt 5.12.6 | Guest-linked `alsashim`, four-channel playback-only loopback and original buffer sizes |

Early SC5000 firmware runs `/usr/JP07/JP07`. Later firmware uses product-named entries under `/usr/Engine`, such as `JP07`, `JC11` or `NH08`; some are links to `Engine`. QEngine checks the old layout and glibc, so a product-named executable alone does not select legacy mode.

## Display and input
Connect to the `Engine VNC` address printed by `run_instance.sh`. QEMU VNC shows the console. `ENGINE_VNC_PORT` in `instance.env` records the forwarded port; QEngine supplies four virtual CPUs for Engine's affinity setup.

The bridge turns VNC pointer events into a uinput touchscreen for Qt's `evdevtouch` handler. Use a viewer that negotiates RichCursor, since Qt 5.9 can crash when a viewer without cursor support disconnects. On Qt 5.5, a blank startup dialog may need a tap to redraw.

Software rendering cannot draw shader-based icons, waveforms or effects. `qt55vnc` supplies transparent raster placeholders for OpenGL textures, preserving the native Qt 5.5 runtime. See [qt55vnc](../../shims/rk3288/qt55vnc/README.md).

## Build
Use the [armv7 guide](../../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) for commands, matching Qt headers and the per-version display/input table. Firmware missing Qt headers needs `LEGACY_QT_HEADERS` from a matching Qt minor version; QEngine still links against the target firmware's libraries.

QEngine omits `-skipFirmwareUpdate` on this path, since Engine 1.x does not accept it. It does not install the contemporary virtual control surface or EGLFS preloads. MONTERO requires Qt 6.

## Limits
Playback and MIDI binding remain unverified, and loopback audio underruns. Input checks cover single taps, not general touchscreen behavior. Prime 4 1.3.2 and 1.3.3 dismiss the startup notice but leave the source browser open; settings navigation on 1.4.1 did not reproduce after a cold boot.

Prime GO 1.6.2 stays up as `JC11`; 1.5.2 retains its battery check even with that identity and quits. Other native battery identities also need care. See [Identity](IDENTITY.md).
