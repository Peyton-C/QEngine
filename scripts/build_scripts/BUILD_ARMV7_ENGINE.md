# armv7 Engine OS
QEngine builds RK3288 Engine OS firmware for QEMU. Install Docker, QEMU, Python 3 and e2fsprogs, then point `--firmware` at an update image, a zipped image, or a desktop updater. QEngine detects the application layout and builds the matching runtime.

## Build
```sh
scripts/build_scripts/new_instance.sh --name sc5000-1.2.0 \
    --product-code JP07 --firmware SC5000Prime1.2.0Updater-Mac.zip
scripts/qemu/run_instance.sh --name sc5000-1.2.0
```

For current firmware, use the same commands with its image and instance name. Each instance owns its rootfs, data disk and host ports. QEngine shares the armhf kernel between instances and builds it on demand.

| Option | Effect |
| --- | --- |
| `--product-code JP07` | Selects the device identity; QEngine records it across `--force` rebuilds |
| `--force` | Rebuilds the rootfs and keeps the data disk |
| `--montero` | Builds MONTERO for Qt 6 Engine; unavailable on the legacy Qt 5 path |
| `--size BYTES` | Changes the rootfs size from its 4 GiB default |

## Legacy firmware
QEngine recognises `/usr/JP07/JP07` and `/usr/Engine/JP07` and builds `dtshim` and `qmlrotate` against that firmware's libraries and Qt headers. It installs a static VNC-to-touch bridge, selects software rendering, rotates the UI to landscape and supplies four virtual CPUs. It keeps the vendor Mali stack and does not inject the newer `-skipFirmwareUpdate` option.

Connect your VNC viewer to the `Engine VNC` address printed by `run_instance.sh`. The separate QEMU VNC address shows the guest console. Use a viewer that negotiates RichCursor; Qt 5.9 can crash when a client without cursor support disconnects.

| Firmware tried as JP07 | Application | Display and input |
| --- | --- | --- |
| SC5000 1.2.0 | `/usr/JP07/JP07` | Landscape UI and touch taps |
| SC5000 1.2.2 | `/usr/Engine/JP07` | Landscape UI and touch taps |
| SC5000 1.3.1 | `/usr/Engine/JP07` | Landscape UI and touch taps |

| Legacy `instance.env` setting | Default |
| --- | --- |
| `ENGINE_VNC_PORT` | A host port derived from the instance name, forwarded to the touch bridge on guest port 5900 |
| `SMP` | `4`, required by Engine's CPU-affinity setup |
| `DISPLAY_MODE` | `vnc`, for the guest console |

Colored icons use shaders that the software renderer cannot draw. Loopback audio underruns, playback and MIDI binding remain unverified, and touch testing covers single taps. Dismiss missing-controller notices with Ignore; this path does not provide a virtual control surface. Other products and firmware versions on this path are untried. `Ctrl+,` opens the settings menu in the tested 1.2.0 and 1.2.2 guests; its binding exists but was not verified on 1.3.1.

## Later firmware
Later Engine builds use EGLFS/KMS and Mesa with virgl in a GL display mode. QEngine replaces the Mali stack on Qt 5.15.2 firmware and adds the matching virgl driver on firmware that already ships Mesa. Use `--no-gl` to rasterize on the guest CPU. HeadRush uses this builder too; see [BUILD_HEADRUSH.md](BUILD_HEADRUSH.md).

QEngine installs `touchbridge`, `midisurface`, the devicetree shim and the application override. The data disk uses the single `az01-internal` partition, the same layout as Gen 1 MPC. Direct use of `build_armv7_engine_rootfs.sh` defaults to the shared `build/rootfs_out.img`; use instances to keep devices apart.

See [INSTANCES.md](INSTANCES.md) for instance settings and [BUILDING.md](../../docs/BUILDING.md) for the runtime mechanisms.
