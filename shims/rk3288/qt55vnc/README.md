# qt55vnc
`qt55vnc` serves Engine's native Qt 5.5 offscreen display over VNC. QEngine builds a matching software scenegraph plugin and keeps the firmware's Qt and `airQuick` libraries.

| Component | Role |
| --- | --- |
| Software scenegraph plugin | Supplies the `customcontext` backend selected by Engine |
| `qt55vnc.so` | Captures the raster backing store and serves raw RGB32 over RFB 3.3 on guest loopback port 5901 |
| `vnctouchbridge` | Forwards the display and converts pointer events into evdev touch on guest port 5900 |

Build an instance normally. QEngine selects this path when the firmware contains Qt 5.5.1. Connect to the Engine VNC address printed by `run_instance.sh` and dismiss the missing-hardware notices. A startup dialog can appear blank until a tap redraws it.

OpenGL texture IDs receive transparent raster placeholders. Shader-drawn waveforms and effects are unavailable; playback and MIDI remain unverified.
