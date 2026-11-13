# mpcsurface
`mpcsurface` supplies the panel MPC, Force and MPC Sample expect, with pads, buttons, the dial, Q-Links and the Sample's knobs. The builder preloads it into the application; it selects the panel from the instance's product code. It does not emulate Key keybeds or MPCe pad expression.

## Commands
Write one command per line to `/run/mpcsurface.fifo` in the guest:

```sh
echo 'button play' > /run/mpcsurface.fifo
```

| Command | What |
| --- | --- |
| `button <name> [ms]` | Press and release a button |
| `hold <name>`, `release <name>` | Keep a button down, as for `shift`, and let it go |
| `pad <1-16> [velocity] [ms]` | Strike a pad, pad 1 at the bottom left; on a Force `pad <1-64>`, pad 1 at the top left |
| `turn dial <steps>`, `turn q1 <steps>` | Turn the data dial or Q-Link 1 to 4, 1 to 8 on a Force, negative for anticlockwise |
| `touch q1 [ms]` | Touch a Q-Link without turning it |
| `list` | Log the button names this product has |
| `press <ch> <note> [ms]` | Raw note on and off, channels from 0 |
| `on <ch> <note> [vel]`, `off <ch> <note>` | Raw note on, raw note off |
| `cc <ch> <cc> <value>` | Raw control change |
| `sysex <hex bytes>` | Raw SysEx |

Buttons have names on `ACVA3`, `ADA2`, `ADA2S` and `AC50`. On other panels, use raw messages with that panel's note numbers. A Force starts in clip-launch mode; send `button note` before playing pads without loaded clips. Force pad 1 is at the top left; MPC pad 1 is at the bottom left.

| `ACVA3` buttons | |
| --- | --- |
| Transport | `play` `stop` `rec` `overdub` `play-start` `locate` |
| Editing | `menu` `main` `shift` `undo` `copy` `erase` `note-repeat` `full-level` `16-level` `tc` `automation` `plus` `minus` `dial` |
| Pad banks | `bank-a` `bank-b` `bank-c` `bank-d` |
| Modes | `sample-edit` `pad-mixer` `track-mixer` `program-edit` `song` `arrange` `grid` `next-seq` `track-mute` `pad-color` `pad-perform` `xyfx` |
| Q-Link modes | `qlink-project` `qlink-track` `qlink-pad-scene` `qlink-pad-param` `qlink-screen` |

| Force buttons | |
| --- | --- |
| Transport | `play` `stop` `rec` `stop-all` |
| Modes | `menu` `matrix` `note` `launch` `step-seq` `mixer` `clip` `master` `knobs` `navigate` `arp` |
| Editing | `shift` `select` `undo` `copy` `delete` `edit` `load` `save` `plus` `minus` `tap-tempo` `dial` |
| Grid | `up` `down` `left` `right` `launch-1` to `launch-8` `column-1` to `column-8` `mute-1` to `mute-8` |
| Track row | `mute` `solo` `rec-arm` `clip-stop` `assign-a` `assign-b` |

The Force map follows [IamForce](https://github.com/TheKikGen/MPCLiveXplore-libs). Only `menu`, `matrix`, `mixer`, `note`, `play` and `stop` were checked here on `ADA2S`; the remaining named controls are unverified. Names describe observed screen or lamp behavior without a real panel comparison.

## MPC Sample
The Sample presents its UART panel as a sound card named `Control Surface`. With product code `AC50`, the shim supplies that interface and accepts these controls:

| Command | What |
| --- | --- |
| `button <name> [ms]`, `hold`, `release` | `sample` `seq` `pad-fx` `knob-fx` `shift` `pad-bank` `erase` `note-repeat` `screen-1` `screen-2` `screen-3` `chop` `mute` `loop` `16-levels` `sample-select` `tap-tempo` `minus` `plus` `sample-rec` `seq-rec` `stop` `play` `dial` |
| `pad <1-16> [velocity] [ms]` | Strike a pad |
| `turn dial <steps>` | The data dial |
| `set <k1\|k2\|k3\|volume\|fader> <0-127>` | Move a knob or the fader to a position |

The Sample map follows [virtual-mpc-sample](https://github.com/orrpunk-art/virtual-mpc-sample). Pads, screen buttons, `seq`, `play`, `sample-select`, the dial and volume were checked here on 1.3.0; the remaining controls are unverified.

## Settings
Set these in the application's service environment, `acvs.service` for MPC or `ac50.service` for MPC Sample.

| Variable | Effect |
| --- | --- |
| `MPCSURFACE_VERBOSE=1` | Logs the application's panel messages |
| `MPCSURFACE_LOG=/data/surface.log` | Selects the log file |
| `MPCSURFACE_VOLUME` | Sample's startup volume, default 100; `set volume` changes it at runtime |

The Sample learns its software volume from the panel. K1 to K3 and the fader are reported only when moved, since the application applies them to the current page.

## Limits
On `ACVA3`, raw note `0x58` on channel 0 stops MPC listening to the panel until `acvs.service` restarts. Named buttons avoid that message.

The 3.10.0 checks cover each Gen 1 and Gen 2 MPC panel, including pad input and identity binding. Key defaults can be nearly silent because the Fabric instrument content is absent from the updater. See [MPC](../../scripts/build_scripts/BUILD_MPC.md) and [MPC Sample](../../scripts/build_scripts/BUILD_MPC_SAMPLE.md) for build and audio limits.
