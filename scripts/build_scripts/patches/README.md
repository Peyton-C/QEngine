# Patches
| Patch | Upstream | File(s) touched | License of those files |
|---|---|---|---|
| `qemu-egl-headless-macos.patch` | `utmapp/qemu` @ `7311c3651` (`utm-edition`, QEMU 10.0.12) | `ui/egl-helpers.c`, `ui/meson.build` | LGPL-2.1-or-later; GPL-2.0-or-later |
| `angle-standalone-dylib-fallback.patch` | `utmapp/WebKit` @ `ed78ab6e` | `Source/ThirdParty/ANGLE/src/common/system_utils.cpp` | BSD-3-Clause |
| `qtwayland-6.7.2-engine.patch` | `qtwayland-everywhere-src-6.7.2` (release tarball) | `src/client/*`, `src/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp` | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only; `CMakeLists.txt` BSD-3-Clause |


Another patch to bake the absolute paths in for libepoxy's ANGLE `dlopen()` is applied by `build_virgl_qemu_macos.sh`.

## Licensing
**The repository's root `LICENSE` (MIT) does not cover this directory.** Each patch is offered under the license of the file it modifies, as its header states.