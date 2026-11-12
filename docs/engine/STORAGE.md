# Storage
Engine keeps application data and user settings under `/data`. QEngine gives each instance its own rootfs and data disk; kernels are shared per architecture.

## Paths
| Path | Contents |
| --- | --- |
| `/usr/Engine/Engine` | Application in later firmware |
| `/usr/JP07/JP07`, `/usr/Engine/<CODE>` | Early product-specific application entries |
| `/usr/Engine/Scripts/` | Firmware launch wrappers and prerequisite scripts |
| `/usr/qt/lib` | Qt libraries in Qt 5 firmware |
| `/data/AppDataUser/AIR Music Technology/Engine/DeviceConfiguration.json` | Audio/MIDI device configuration |
| `/data/AppDataUser/AIR Music Technology/Engine/State/` | `PlanckClientState.json`, `PlanckEngineState.json` and `PlanckGuiState.json` |
| `/data/AppDataUser/AIR Music Technology/Engine/CrashReportDB/` | Native crash-report database |
| `/data/.config/<product>.user.settings/AIR Music Technology/Engine.conf` | Qt INI settings, including onboarding, streaming toggles and ignored update versions |

The product settings namespace follows the selected identity, such as `jc11.user.settings` or `jp11.user.settings`. It does not hold audio/MIDI device selection. Engine builds its available-device list by scanning ALSA; editing `DeviceConfiguration.json` cannot make a rejected sound card appear.

## Partitions
| Platform | Layout |
| --- | --- |
| RK3288 | `az01-internal` data partition, with writable `/etc` and `/var` overlays |
| RK3588 | Read-only `/`, separate `/data` and `/factory`, and `/content` and `/secure-media` mountpoints |

On RK3588, static systemd mount units locate partitions by PARTUUID. The `az0x-*-mkfs` services can format an unlabeled partition before mounting it. `/var/lib` binds from `/data/system/var-lib`. The launch scripts apply ext4 per-directory encryption to `/data/downloads` and `/data/stems` with `fscryptctl`; this is not whole-partition LUKS encryption.

Use [instances](../../scripts/build_scripts/INSTANCES.md) to keep disks and identities together. Stop an instance before editing its images with `debugfs`. To change files in a live read-only guest, remount `/` writable first.

## Services
| Service or wrapper | Role in QEngine |
| --- | --- |
| `engine.service` | Starts Engine with the instance's display environment and preloads |
| `jp07.service` | Application service in early SC5000 firmware |
| `runengine` → `engine` | Firmware launch chain; modern scripts also set up prerequisites and encrypted directories |
| `az01-libmali-setup.service` | Disabled where it would probe absent Mali hardware |
| `touchbridge` / `vnctouchbridge` | Supplies touchscreen events for the selected display path |
| `midisurface` | Supplies the virtual control surface on the EGLFS runtime |

QEngine blanks the root password for serial-console access. It configures application services rather than relying on the stock launch environment. `quitshim` turns service shutdown into Engine's clean exit on the EGLFS runtime. The legacy path installs its own launcher and service.

## Telemetry
QEngine blocks inMusic crash and analytics endpoints in every built rootfs. The host list lives only in `rootfs_steps/block_telemetry.sh`; add new endpoints there. See [Telemetry](../BLOCKING_TELEMETRY.md). A crash-report process or local database can still exist when network reporting is blocked.
