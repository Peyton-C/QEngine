# QEngine UI concept
A concept for a front end to QEngine, drawn with Dear ImGui on SDL3 GPU and laid out like a VM manager. It lists the instances under `build/instances`, builds new ones and starts and stops them, and draws the running one as its front panel from a layout file, played over MIDI with the guest's screen in it.

## Build
```
cmake -S ui -B ui/build -G Ninja
cmake --build ui/build
ui/build/qengine-ui
```

A system SDL3 is used when CMake finds one, otherwise it is fetched with Dear ImGui, nlohmann/json and the Material Design Icons font, which draws the UI's own buttons.

## Devices
The sidebar lists every instance, with a button to start or stop it, and its context menu edits, rebuilds, shows or deletes it. The button at the toolbar's left, or Cmd+Ctrl+S, hides it and gives the panel the whole window. The plus button, or Cmd+N, opens the New Device sheet: pick a product, open its update image, and give it a name, and the UI runs `new_instance.sh` with `--product-code` and `--montero` as chosen. Edit runs it again on an existing instance with `--force`, which keeps `/data`. A JP22 also gets `GPU_MAX_OUTPUTS=3` in its `instance.env`.

Builds and VMs run detached, with their output in `build.log` and `console.log` in the instance's directory, so they carry on when the UI closes. The UI reads what runs from the process table, so a VM started from a terminal is listed as running too, and the console button shows both logs. Start runs `run_instance.sh` with `--display vnc`, or `egl-vnc` where the instance records a GL display, since the panel reads the screen over VNC.

A product with no layout shows its screen on its own. `--instance NAME` selects an instance at start, and `--new` opens the sheet, on a product with `qengine-ui JP24 --new`.

## Layouts
Each device is one file in `layouts/`, in millimetres from the faceplate's top left, so every device shares the same drawing code and a button is the same size on all of them. `defaults` sets each kind's style once per device, and `templates` lets a two deck unit draw its deck once and place it twice. An optional `front` block adds the front panel, drawn below the top with its own size, body and elements, and sharing the top's defaults and templates. The app reloads a layout when its file is saved.

A product that differs only in finish or printing is a `variants` entry in its sibling's file, listed as its own device. It merges over the base's settings, swaps colours everywhere with `colors`, patches or hides single elements by id with `elements`, and adds its own with `add`. A placed template can also take a `patch`, so deck 2 can differ from deck 1. The Prime GO+ (`JP11S`) is the Prime GO in silver this way, and the Prime 4+ (`JC11S`) is a Prime 4. The sidebar's Layouts section draws one on its own, without a guest, and a layout name or product code on the command line opens it, as in `qengine-ui JP24`.

| Kind | What |
| --- | --- |
| `button` | Lit through the cap, the legend, or an LED `ring`, with an optional colour `stripe` |
| `pad` | Velocity pad with face lighting, `"rim": true` for edge lighting, or a colour `strip`; `"quadrants": true` draws four zones, as on Prime 4 G2, MPC Live III and MPC XL |
| `knob`, `encoder` | Ranged or endless, with optional `scale`, `halo`, `arc`, `well` and `metal` cap; an encoder has fluted sides and a dished top, set `ribs` or `"style": "plain"` to change that |
| `fader` | Vertical or `"orient": "h"`, with ticks and a centre `detent` |
| `jog` | Platter that coasts after a flick: brushed metal, `"style": "vinyl"`, or `"style": "display"` with an LED ring and a centre screen |
| `footswitch` | A pedal's stomp switch |
| `switch` | Two or three position slide switch, stepped by clicking |
| `jack` | Audio socket |
| `lcd` | Where the guest's display goes |
| `meter`, `led` | Lamps, with `"round": true` for dot meters |
| `panel`, `frame`, `bracket`, `hatch`, `line`, `text`, `icon`, `screw` | Decoration, with `"pattern": "dots"` for a speaker grille; `hatch` is printed accent marks |

Text sizes are cap heights, as measured off a photo, and `fonts.scale` widens a maker whose printing runs wider than ours.

On a Mac with a Force Touch trackpad, drag or scroll to feel encoder detents and the centre of detented faders and knobs. Bipolar knobs have a centre detent by default. Set `"detent"` to override it.

## Guest
Selecting a running Engine, MPC or MPC Sample instance connects the panel to its MIDI port, the one `run_instance.sh` prints, and the status strip shows how that is going. The panel then speaks the product's own protocol through `midibridge` and `midisurface` in the guest, and Engine lights its lamps back, so Engine takes it for the real control surface. See [Audio and MIDI](../docs/engine/AUDIO_MIDI.md#host-midi).

A control sends what its `midi` field says, in the numbers from the product's assignment file: a `note` for a button or pad, a `cc` or a 14-bit `[upper, lower]` pair for a knob or fader, a `relative` CC for an encoder, and a touch note with a position pair for a jog. Engine lights a control on the note it sends unless `lamp` names another, or four for a split pad's quarters, with `lamp_ch` for another channel and `lamp_type` where an RGB lamp and a simple one share a note. An `led` or an indicator has only a `lamp`, and a `meter`'s `cc` is the one Engine sends its level and peak on, read as the assignment file's `ledCCValues` where the meter lists them as `levels`. A switch or a stepped knob with `values` sends one for each position, a CC's value or a note's velocity, as an FX paddle or a channel assign selector does. A placed template's `midi_channel` gives its controls their channel, and the layout's `midi.channel` the rest. Build each instance for the product whose panel you want to use.

`mpcsurface` answers the guest's handshake and connects without restarting MPC, Force or MPC Sample. A layout with `"midi": {"lamps": "cc"}` lights buttons by CC and pads by Akai's pad colour SysEx. An encoder's `touch` note stays down while you turn it.

MPC and Force layouts send pad notes, data dial turns and mapped Q-Links and buttons. Gen 1 button numbers follow [IamForce](https://github.com/TheKikGen/MPCLiveXplore-libs/blob/master/src/tkgl_midimapper.h). Unmapped controls, Key keybeds and touch strips are visual. The Live III and XL show four MPCe rim colours with a `pad` array of SysEx indices, and send a normal pad hit.

The HeadRush, Sheeran and Alesis layouts show the guest display. Use the screen to control guests with touch support. The physical controls are visual.

Engine only binds the surface when it starts, which a VM started from the UI always gets, since the panel connects as it boots. Knobs and faders are sent when they move, not when connecting, so Engine keeps its own positions until then.

## Screen
Connecting also shows the guest's display in the layout's first `lcd`, read over VNC, and a click or a finger on it reaches the guest as a touch through its usb-tablet. The UI reads QEMU's VNC console on `5900 + VNC_DISPLAY`, which only the `vnc` and `egl-vnc` displays serve, so a VM started from a terminal needs `--display vnc` where it would otherwise open a window. A legacy rootfs draws Engine through its own Qt VNC server instead, and the UI reads that on `ENGINE_VNC_PORT`. The UI has its own small RFB client, which asks for raw pixels since both servers are on localhost, and accepts no password. A VNC viewer can stay connected beside it.

The View menu's Screen in bezel, or `--view screen`, shows the guest's display alone in a frame coloured as the device. A layout's `bezel` sets it, as `"bands": [["#cfc6b6", 1], ["#121315", 2]]` stacks the Prime 4 G2's champagne third over its black, with an optional `edge` and a `border` as a share of the screen's shorter side. Without one the frame takes the body's colour, so the MPC One's is red.

## Screenshots
`--screenshot out.png` renders one frame without a window and quits, and `--view` picks the view for it. With a running guest it waits four seconds for the screen and lamps, or the time you set with `--wait`.

`--overlay photo.png` lays a product photo over the top face at half opacity, to check a layout against it. `--overlay-rect x0,y0,x1,y1` gives the faceplate's pixels in the photo, `--overlay-face front` picks the front panel and `--overlay-alpha` the opacity. Photos are inMusic's, so keep them in `build/` and never commit them.

## Touch
Each finger holds its own control, so two jogs or a fader and a knob can move at once. SDL reports touchscreens itself on Linux and Windows. On macOS 27 AppKit gives a touchscreen's or Sidecar's fingers only to gesture recognizers, and sends the window nothing for a drag, so the UI attaches a recognizer of its own and ignores the clicks macOS makes up from taps. A touch outside the device drives the rest of the window as a mouse. Scrolls turn knobs, faders and platters too, for a trackpad. `--log-input` prints the clicks, scrolls and touches that arrive.
