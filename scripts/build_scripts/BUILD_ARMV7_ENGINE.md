# armv7 Engine OS
QEngine builds RK3288 Engine OS firmware for QEMU. Install Docker, QEMU, Python 3 and e2fsprogs, then point `--firmware` at an update image, a zipped image, or a desktop updater. QEngine detects the application layout and builds the matching runtime.

## Build
```sh
scripts/build_scripts/new_instance.sh --name sc5000-1.0.1 \
    --product-code JP07 --firmware SC5000-Prime-FW-Updater-1.0.1-Mac.zip
scripts/qemu/run_instance.sh --name sc5000-1.0.1
```

For current firmware, use the same commands with its image and instance name. Each instance owns its rootfs, data disk and host ports. QEngine shares the armhf kernel between instances and builds it on demand.

| Option | Effect |
| --- | --- |
| `--product-code JP07` | Selects the device identity; QEngine records it across `--force` rebuilds |
| `--force` | Rebuilds the rootfs and keeps the data disk |
| `--montero` | Builds MONTERO for Qt 6 Engine; unavailable on the legacy Qt 5 path |
| `--size BYTES` | Changes the rootfs size from its 4 GiB default |

## Legacy firmware
QEngine recognises `/usr/JP07/JP07` and a product-named application under `/usr/Engine`, such as `JP07`, `JP13` or `JC16`, and builds `dtshim` and `qmlrotate` against that firmware's libraries and Qt headers. It installs a static VNC-to-touch bridge, selects software rendering, selects the device's landscape rotation and supplies four virtual CPUs. It keeps the vendor Mali stack and does not inject the newer `-skipFirmwareUpdate` option.

SC5000 1.0.x keeps its native Qt 5.5.1 runtime. QEngine builds a matching raster scenegraph plugin and adds `qt55vnc` to serve its offscreen display. OpenGL textures receive transparent raster placeholders, so shader-drawn waveforms and effects are unavailable. The first build downloads the pinned Qt sources; later builds reuse the matching artifacts.

Connect your VNC viewer to the `Engine VNC` address printed by `run_instance.sh`. The separate QEMU VNC address shows the guest console. A Qt 5.5 startup dialog can appear blank until a tap redraws it. Use a viewer that negotiates RichCursor; Qt 5.9 can crash when a client without cursor support disconnects.

| Firmware | Product code | Application | Display and input |
| --- | --- | --- | --- |
| SC5000 1.0.1, 1.0.2, 1.0.3 | JP07 | `/usr/JP07/JP07` | Landscape UI and startup-notice taps after cold boot |
| SC5000 1.2.0 | JP07 | `/usr/JP07/JP07` | Landscape UI and touch taps |
| SC5000 1.2.2 | JP07 | `/usr/Engine/JP07` | Landscape UI and touch taps |
| SC5000 1.3.1 | JP07 | `/usr/Engine/JP07` | Landscape UI and touch taps |
| SC5000 1.4.1 | JP07 | `/usr/Engine/JP07` | Landscape UI; startup notice dismissed and Source closed with single taps |
| SC5000 1.5.2, 1.6.0, 1.6.2 | JP07 | `/usr/Engine/JP07` | Landscape UI; startup notice dismissed and Source closed with single taps after cold boot |
| SC5000M 1.6.0 | JP08 | `/usr/Engine/JP08` | Landscape UI; startup notice dismissed and Source closed with single taps after cold boot |
| SC6000 1.6.2 | JP13 | `/usr/Engine/JP13` | Landscape UI; startup notice dismissed and Source closed with single taps after cold boot; one of two boots stalled before the UI appeared |
| SC6000M 1.6.0 | JP14 | `/usr/Engine/JP14` | Landscape UI; startup notice dismissed and Source closed with single taps after cold boot |
| Prime 2 1.6.2 | JC16 | `/usr/Engine/JC16` | Landscape UI; startup notice dismissed and Source closed with single taps |
| Prime GO 1.6.2 | JC11 | `/usr/Engine/JP11` | Landscape UI as Prime 4; startup notice dismissed and Source closed with single taps |
| Prime 4 1.3.2, 1.3.3 | JC11 | `/usr/Engine/JC11` | Landscape UI; startup-notice touch verified, source browser stays open |
| Prime 4 1.4.1 | JC11 | `/usr/Engine/JC11` | Landscape UI; settings touch navigation worked once, but the shortcut did not reopen settings after a cold boot |
| Prime 4 1.5.2, 1.6.0, 1.6.2 | JC11 | `/usr/Engine/JC11` | Landscape UI; startup notice dismissed and Source closed with single taps |

Pass the product code the application is named after, such as `JP08` for SC5000M or `JC16` for Prime 2. Build Prime GO as `JC11`: as `JP11`, Engine shows a blank screen and quits on its battery check about 20 seconds after starting. Its Qt 5.9 runtime needs a 270-degree correction; Qt 5.12 already renders in landscape on both products. Earlier SC5000 firmware needs the 90-degree correction.

Firmware 1.5.2 and later omit Qt development headers. Extract the headers from an SC5000 1.4.1 instance, then set `LEGACY_QT_HEADERS` when building either version. QEngine checks the Qt minor version and links against the target firmware's libraries.

```sh
mkdir -p build/qt512-headers
debugfs -R 'rdump /usr/qt/include build/qt512-headers' build/instances/sc5000-1.4.1/rootfs.img
LEGACY_QT_HEADERS="$PWD/build/qt512-headers/include" \
    scripts/build_scripts/new_instance.sh --name sc5000-1.5.2 \
    --product-code JP07 --firmware SC5000-1.5.2-Update.img.zip
```

On macOS, use `/opt/homebrew/opt/e2fsprogs/sbin/debugfs` if `debugfs` is outside your `PATH`.

QEngine builds `alsashim` against the guest libraries for the Qt 5.12.6 runtime of the SC media players, `JP07`, `JP08`, `JP13` and `JP14`. Prime 2 and Prime 4 start without it. It converts PCM formats and rates, exposes four playback channels, hides loopback capture and keeps Engine's original buffer sizes. This configuration is automatic; `QEMU_AUDIO=off` is not required.

| Legacy `instance.env` setting | Default |
| --- | --- |
| `ENGINE_VNC_PORT` | A host port derived from the instance name, forwarded to the touch bridge on guest port 5900 |
| `SMP` | `4`, required by Engine's CPU-affinity setup |
| `DISPLAY_MODE` | `vnc`, for the guest console |

Colored icons use shaders that the software renderer cannot draw. Loopback audio underruns, playback and MIDI binding remain unverified, and touch testing covers single taps. Dismiss missing-controller notices with Ignore; tap the hardware warning to continue on 1.0.1. This path does not provide a virtual control surface. Other products and firmware versions on this path are untried. `Ctrl+,` opens the settings menu in the tested 1.2.0 and 1.2.2 guests; its binding exists but was not verified on 1.3.1.

## Later firmware
Later Engine builds use EGLFS/KMS and Mesa with virgl in a GL display mode. QEngine replaces the Mali stack on Qt 5.15.2 firmware and adds the matching virgl driver on firmware that already ships Mesa. Use `--no-gl` to rasterize on the guest CPU. HeadRush uses this builder too; see [BUILD_HEADRUSH.md](BUILD_HEADRUSH.md).

QEngine installs `touchbridge`, `midisurface`, the devicetree shim and the application override. The data disk uses the single `az01-internal` partition, the same layout as Gen 1 MPC. Direct use of `build_armv7_engine_rootfs.sh` defaults to the shared `build/rootfs_out.img`; use instances to keep devices apart.

See [INSTANCES.md](INSTANCES.md) for instance settings and [BUILDING.md](../../docs/BUILDING.md) for the runtime mechanisms.
