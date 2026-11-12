# HeadRush
QEngine boots HeadRush Prime and VX5 with Docker, QEMU, Python 3 and e2fsprogs. Point `--firmware` at the update image or desktop updater; QEngine selects the architecture and builder from its contents.

| Device | Code | Firmware tried | Architecture | Display | Limits |
| --- | --- | --- | --- | --- | --- |
| Prime | `HG06` | 5.2.0 | armv7, RK3288 | Landscape with touch | Silent loopback audio |
| VX5 | `HV03` | 1.2.2 | arm64, RK3566 | 160×128, landscape | Input from a WAV file, no LEDs or phantom power |

## Build
```sh
scripts/build_scripts/new_instance.sh --name headrush-vx5-1.2.2 --product-code HV03 --firmware HeadRush-VX5-1.2.2.img
scripts/qemu/run_instance.sh --name headrush-vx5-1.2.2 --display vnc
```

Use a different instance name for each device and firmware version. `--force` rebuilds the rootfs and keeps the data disk. See [INSTANCES.md](INSTANCES.md) for display options and host ports.

VX5 uses the shared JUCE builder, `build_mpc_rootfs.sh`, and `HV03.service`. Prime uses the armv7 Engine builder and `evil.service`. VX5 needs no Qt or EGL replacement and no rotation shim. Both use QEngine's generic kernel, not the device kernel in the updater.

## Audio
Prime gets a kernel loopback card, renamed `HV01`, which `pcmhold` keeps at 8 channels out and 2 in at 96000 Hz. Capture is silent and playback is discarded, so Prime makes no sound on the host.

VX5 plays through the host. It opens `/dev/snd/by-path/platform-sound` as stereo S32_LE at 48000 Hz in 128-frame periods, which a QEngine udev rule points at `snd-dummy`, loaded by `qengine-vx5-audio.service` with the `qengine-ac50` model. `alsashim`, preloaded into `HV03.service`, copies VX5's output onto QEMU's sound card and unmutes it. QEMU on macOS cannot record, so `alsashim` replaces what VX5 hears with `/data/input.wav`, looped and resampled to 48000 Hz, or silence when there is none. Copy a WAV there, for instance over SSH as in [REMOTE_ACCESS.md](../../docs/REMOTE_ACCESS.md), and run `systemctl restart HV03` in the guest.

The VX5 unit also disables glibc's small-block allocation cache and zero-fills fresh allocations to avoid a DSP startup crash in 1.2.2.

If the generic kernel reports that `snd-aloop` or `snd-dummy` is missing, shut down the guest and rebuild the shared kernel and initrd for its architecture:

```sh
scripts/build_scripts/get_kernel.sh --arch arm64
```

## Controls
Prime is driven by touch. VX5 reads its panel as input devices, which `vx5surface` creates, along with the USB MIDI port `f_midi` that VX5 binds by name. Drive both from the guest:

```sh
echo 'press autotune' > /run/vx5surface.fifo
```

| Command | Does |
| --- | --- |
| `press <switch> [ms]`, `hold <switch>`, `release <switch>` | Switches: `ft1` and `ft2` (footswitches A and B), `preset`, `key`, `autotune`, `harmony`, `fx`, `select` (encoder 1's push), `phantom` |
| `turn <1-3> <steps>` | Encoders; 2 and 3 are Retune and Humanize on the preset screen. Negative steps turn anticlockwise |
| `volume <0-100>` | The volume knob. It starts at 80; at 0 VX5 plays nothing |
| `pc <n> [ch]` | Program change, preset `n + 1` |
| `cc <cc> <value> [ch]` | Control change |
| `note <note> <vel> [ch]` | Note on, or off at velocity 0 |

VX5 1.2.2 acts on these MIDI messages, on any channel:

| Message | Effect |
| --- | --- |
| Program change | Selects a preset, offset by 128 per bank |
| CC 0, CC 32 | Bank MSB and LSB |
| CC 16 | 1 for the previous preset, 2 for the next |
| CC 17 | Talk mode on at 64 and above, off below |

The LEDs and the phantom-power supply are not emulated, so those paths still log assertions. Newer VX5 firmware has not been tried.
