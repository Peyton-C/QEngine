# Alesis drum modules
QEngine boots Prime Drum Module 1.5.2.3 as `LDMD` and Core Drum Module 1.5.2.3 as `LDMF`, on RK3288 armv7. Both reach the kit screen with touch and bundled kits, but have no audio device or full factory sample library. Install Docker, QEMU, Python 3, e2fsprogs and `file`; see [Build](../../docs/BUILDING.md).

## Build
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name ldmd-1.5.2.3 \
    --product-code LDMD --firmware /path/to/drummodule-1.5.2.3-update.img
scripts/qemu/run_instance.sh --name ldmd-1.5.2.3
```

For the Core, pass `--product-code LDMF`. QEngine detects `drummodule` and uses `build_mpc_rootfs.sh`. The application is `/usr/bin/DrumModule`, started by `alesis-drummodule.service` with `dtshim` preloaded.

| Device | Code | Support |
| --- | --- | --- |
| Prime Drum Module | `LDMD` | 1.5.2.3 boots to the kit screen with touch |
| Core Drum Module | `LDMF` | 1.5.2.3 boots to the kit screen with touch and 75 factory kits |

## Storage
Each instance gets `emmc.img` for `/data` and a blank ext4 `sdcard.img` for `/sdcard`. The SD card also holds writable state for `/sdcard/bfd`, whose lower layer contains bundled kits and presets from `/usr/share/Alesis/ldmd-content-update`. The full sample library ships on the physical card, outside the update image.

| Build change | Purpose |
| --- | --- |
| Content-generator wrappers | Preload `dtshim` so generators select the LDMD storage layout |
| `90-qengine-internal-sd.rules` | Exposes the virtual card at the RK3288 SD path expected by the firmware |
| `qengine.conf` mount drop-ins | Remove devicetree conditions that systemd cannot satisfy under QEMU |

Without the SD card, the application stops at "File system problem". `--force` preserves both writable disks; [Instances](INSTANCES.md) covers backups and resets.

The Core has no SD card. It keeps its content on an eMMC partition, which QEngine supplies as `content.img`, a GPT disk whose partition carries the PARTUUID the guest's `content.mount` asks for. The guest mounts it read-only on `/content` and binds it over `/sdcard`. The partition holds only an empty `bfd` directory, the lower layer of the `/sdcard/bfd` overlay, and the 75 Core kits come from `/usr/share/Alesis/ldmf-content-update` in the rootfs. The app checks for the partition, not its contents. `dtshim` serves `chosen/inmusic,emmc-drum-content` for `LDMF` only.

## Limits
The update image carries kit definitions but no samples, so kit playback without the factory library is unverified on either module, and the guest has no working audio device. The pad-controller firmware tool reports version 0.0.0.000 without a real controller and carries on. QEngine does not install MPC's virtual panel for this application.
