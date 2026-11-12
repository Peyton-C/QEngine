# QEngine UI concept
A concept for a front panel UI, drawn with Dear ImGui on SDL3 GPU. It draws a device from a layout file and lets you press, turn and drag its controls. Nothing reaches a guest yet.

## Build
```
cmake -S ui -B ui/build -G Ninja
cmake --build ui/build
ui/build/qengine-ui primego
```

A system SDL3 is used when CMake finds one, otherwise it is fetched with Dear ImGui and nlohmann/json.

## Layouts
Each device is one file in `layouts/`, in millimetres from the faceplate's top left, so every device shares the same drawing code and a button is the same size on all of them. `defaults` sets each kind's style once per device, and `templates` lets a two deck unit draw its deck once and place it twice. An optional `front` block adds the front panel, drawn below the top with its own size, body, photo and elements, and sharing the top's defaults and templates. The app reloads a layout when its file is saved.

A product that differs only in finish or printing is a `variants` entry in its sibling's file, listed as its own device. It merges over the base's settings, swaps colours everywhere with `colors`, patches or hides single elements by id with `elements`, and adds its own with `add`. A placed template can also take a `patch`, so deck 2 can differ from deck 1. The Prime GO+ (`JP11S`) is the Prime GO in silver this way, and the Prime 4+ (`JC11S`) is a Prime 4. Pass a product code in place of a layout name to open one, as in `qengine-ui JP24`.

| Kind | What |
| --- | --- |
| `button` | Lit through the cap, the legend, or an LED `ring`, with an optional colour `stripe` |
| `pad` | Velocity pad, lit across its face, round its rim, or along a colour `strip`; `"quadrants": true` splits it into four, as MPC pads are |
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

## Photos
Product photos are inMusic's and are never committed. Put them in `build/product_photos`, or point `--photos` elsewhere, and a layout's `photo.rect` maps its faceplate. The Photo view shows the photo with the screen and pressed controls drawn over it. Trace lays the drawing over the photo, which is how a new layout is checked.

`--screenshot out.png` renders one frame without a window and quits, and `--view` picks the view for it.

## Touch
Each finger holds its own control, so two jogs or a fader and a knob can move at once. SDL reports touchscreens itself on Linux and Windows. On macOS 27 AppKit gives a touchscreen's or Sidecar's fingers only to gesture recognizers, and sends the window nothing for a drag, so the UI attaches a recognizer of its own and ignores the clicks macOS makes up from taps. A touch outside the device drives the sidebar as a mouse. Scrolls turn knobs, faders and platters too, for a trackpad. `--log-input` prints the clicks, scrolls and touches that arrive.
