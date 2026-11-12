# Display and input
QEngine uses EGLFS/KMS with Mesa where the guest runtime supports it, and software rendering through Qt on legacy firmware. The application keeps the firmware's Qt libraries.

## Display paths
| Runtime | Renderer | VNC server | Pointer input |
| --- | --- | --- | --- |
| Modern, Qt 6 | EGLFS/KMS and Mesa | QEMU | `touchbridge` reads QEMU's tablet |
| Classic, Qt 5.15.2 | EGLFS/KMS with a matching KMS integration and Mesa replacing Mali | QEMU | `touchbridge` reads QEMU's tablet |
| Legacy, Qt 5.9 to 5.15 | Qt software scenegraph and VNC platform | Qt, through `vnctouchbridge` | Bridge translates VNC pointer messages |
| Legacy, Qt 5.5.1 | Matching raster scenegraph and offscreen platform | `qt55vnc`, through `vnctouchbridge` | Bridge supplies touch to the offscreen window |

Qt 6 firmware has no VNC platform plugin. `QT_QPA_PLATFORM=vnc` cannot replace QEMU's display on that runtime. Legacy instances print a separate `Engine VNC` address; their QEMU VNC address shows the console.

## Mesa
| Rootfs layout | QEngine installation |
| --- | --- |
| `dri` | Adds `virtio_gpu_dri.so` alongside the vendor drivers |
| `gallium` | Replaces the matching `libgallium-<version>.so`, keeping the vendor copy as `.vendor` |
| `mali` | Replaces `libmali.so.14.0` with a Mesa-backed stand-in, redirects EGL/GLES links and installs Qt's matching KMS integration |
| Legacy | Keeps vendor GL libraries and selects software rendering |

The builders detect the library layout rather than assuming it from the architecture. Virgl carries guest rendering to the host GPU; `run_instance.sh --no-gl` selects CPU rendering. See [macOS virgl](../MACOS_VIRGL.md) for host setup.

## Modesetting
`drmatomic` rewrites ARGB8888 framebuffers as XRGB8888 for virtio-gpu's primary plane and replaces Qt's legacy modeset/page-flip calls with atomic commits. It exports both `ioctl` and the armv7 time64 entry point. The Pi does not preload it, since its DRM driver does not need the virtio-gpu workaround.

QEngine rewrites screen configuration for the virtual connector and serves panel rotation as zero through `dtshim`. Physical panel rotation is not a suitable value for a virtual framebuffer. Legacy Qt may also need `qmlrotate` to correct the application's own content rotation.

## Touch
Engine expects touch events. On EGLFS, `touchbridge` finds QEMU's tablet by its device name and absolute-axis capabilities, then emits uinput multitouch events. It does not depend on a fixed `/dev/input/eventN` number. Qt consumes the synthetic device through `evdevtouch`.

On legacy firmware, `vnctouchbridge` translates the VNC client's pointer messages instead. Use the instance's Engine VNC port so those messages pass through the bridge. A rendered frame alone does not establish input support; the [legacy guide](LEGACY.md) and [armv7 build guide](../../scripts/build_scripts/BUILD_ARMV7_ENGINE.md) describe the checked interactions and remaining limits.

## Pi
The Pi uses Mesa's v3d/vc4 drivers. `touchbridge --mouse` supplies touch from a USB mouse and `cursorshim` draws a pointer under EGLFS. Optional Wayland staging adds a matching Qt plugin that supplies touch from mouse clicks and reports a configured window size. Follow the [Pi guide](../../scripts/pi/README.md) for screen scaling, sway and keyboard input.
