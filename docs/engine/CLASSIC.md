# Classic Engine OS
Classic Engine OS runs on RK3288 armv7 devices, covering Engine 2.4.0 to 4.3.4. It uses Buildroot, Qt 5.15.2 and proprietary Mali libraries. QEngine replaces the graphics stack around Engine so it can render through virtio-gpu.

## Graphics
Qt links directly to `libmali.so.14.0`, including image-format plugins that need no GL themselves. Installing Mesa beside the blob leaves those dependencies on the vendor driver. QEngine preserves the blob as `.vendor`, installs a Mesa-backed stand-in under its name and redirects the EGL/GLES links to Mesa.

QEngine also builds the EGLFS/KMS integration against the firmware's Qt libraries. The vendor Qt only has the Mali integration; replacing the GL libraries alone does not give it a display backend. See [Display and input](DISPLAY.md).

## Runtime
| Component | Handling |
| --- | --- |
| Application | `/usr/Engine/Engine`; some firmware retains a product-named entry |
| Qt | Vendor Qt 5.15.2 under `/usr/qt` |
| Display | EGLFS/KMS, Mesa and `drmatomic`; QEMU provides VNC |
| Input | `touchbridge` supplies touch from QEMU's tablet |
| Data | `az01-internal` partition, with writable overlays for `/etc` and `/var` |
| Identity and MIDI | `dtshim`, `alsashim` and `midisurface` |
| MONTERO | Unavailable, requires Qt 6 |

Qt 5.15.2 alone does not select this path. Mixstream Pro 2.3.x and SC Live 2 2.3.3 use glibc 2.32, too old for QEngine's current Mesa and preloads, so they use the [legacy runtime](LEGACY.md).

## Limits
The repository records startup-dialog and library taps on Prime GO 2.4.0, 3.4.0 and 4.0.0 built as `JC11`, using EGLFS and `touchbridge`. Those checks do not establish every gesture or playback. The native `JP11` identity has battery and display problems; use [Identity](IDENTITY.md) for the caveats.

Use the [armv7 build guide](../../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) for commands and the [device tables](../../README.md#emulated-controllers) for booted versions. [Storage](STORAGE.md) covers settings and services; [Audio and MIDI](AUDIO_MIDI.md) covers the shared control-surface path.
