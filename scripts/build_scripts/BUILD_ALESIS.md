# Alesis drum modules
QEngine boots Prime Drum Module 1.5.2.3 as `LDMD` on RK3288 armv7. It reaches the kit screen with touch and bundled kits, but has no audio device or full factory sample library. Install Docker, QEMU, Python 3, e2fsprogs and `file`; see [Build](../../docs/BUILDING.md).

## Build
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name ldmd-1.5.2.3 \
    --product-code LDMD --firmware /path/to/drummodule-1.5.2.3-update.img
scripts/qemu/run_instance.sh --name ldmd-1.5.2.3
```

QEngine detects `drummodule` and uses `build_mpc_rootfs.sh`. The application is `/usr/bin/DrumModule`, started by `alesis-drummodule.service` with `dtshim` preloaded.

| Device | Code | Support |
| --- | --- | --- |
| Prime Drum Module | `LDMD` | 1.5.2.3 boots to the kit screen with touch |
| Core Drum Module | `LDMF` | Build refused; required content partition is not supplied |

## Storage
Each instance gets `emmc.img` for `/data` and a blank ext4 `sdcard.img` for `/sdcard`. The SD card also holds writable state for `/sdcard/bfd`, whose lower layer contains bundled kits and presets from `/usr/share/Alesis/ldmd-content-update`. The full sample library ships on the physical card, outside the update image.

| Build change | Purpose |
| --- | --- |
| Content-generator wrappers | Preload `dtshim` so generators select the LDMD storage layout |
| `90-qengine-internal-sd.rules` | Exposes the virtual card at the RK3288 SD path expected by the firmware |
| `qengine.conf` mount drop-ins | Remove devicetree conditions that systemd cannot satisfy under QEMU |

Without the SD card, the application stops at "File system problem". `--force` preserves both writable disks; [Instances](INSTANCES.md) covers backups and resets. LDMF needs a separate eMMC content partition that neither disk provides.

## Limits
Kit playback without the full library is unverified, and the guest has no working audio device. The pad-controller firmware tool reports version 0.0.0.000 without a real controller and carries on. QEngine does not install MPC's virtual panel for this application.
