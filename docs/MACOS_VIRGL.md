# macOS virgl
QEngine's macOS QEMU build renders guest GL on the host GPU through virgl and serves the picture over VNC. Use it for modern and classic Engine firmware; legacy Engine keeps its software renderer. This build supports Apple Silicon.

## Requirements
Install Xcode with its macOS SDK, Homebrew, Git, Python 3 and rsync. Install the build tools and libraries:

```sh
brew install meson ninja pkgconf glib pixman gnutls libusb usbredir
```

Run the dependency check from the repository root:

```sh
scripts/build_scripts/build_virgl_qemu_macos.sh --stage deps
```

If it reports a missing Metal toolchain, install that Xcode component with `xcodebuild -downloadComponent MetalToolchain`, then rerun the check.

## Build
```sh
scripts/build_scripts/build_virgl_qemu_macos.sh
```

The script builds pinned UTM QEMU, ANGLE, libepoxy and virglrenderer sources under `build/qemu-virgl/`. It installs a local `qemu-system-aarch64`, which serves both arm64 and armv7 guests. Homebrew's QEMU remains untouched.

| Path under `build/qemu-virgl/` | Contents |
| --- | --- |
| `prefix/bin/qemu-system-aarch64` | QEMU executable |
| `prefix/lib/` | Matching graphics libraries |
| `src/`, `work/` | Sources and build trees |
| `logs/<stage>.log` | Build output, `all.log` for a full run |
| `stamps/` | Records used to skip unchanged stages |

Rerun the command after an interruption; completed stages are reused when their inputs match. `--jobs N` limits parallel work and `--force` rebuilds selected stages. Keep the prefix with its libraries when using the executable.

## Run
The launcher prefers the local build automatically. Start an existing instance with:

```sh
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display egl-vnc
```

Connect a VNC viewer to the address printed by the launcher. Its TCP port is `5900 + VNC_DISPLAY`. Use the viewer for screenshots; QEMU monitor `screendump` has no surface in `egl-headless` mode.

New macOS instances record `egl-vnc` when this build exists. Existing instances keep their recorded mode; set `DISPLAY_MODE=egl-vnc` in `instance.env` to make the choice persistent. `--gl` also promotes `cocoa` to `egl-vnc`, while `--no-gl` selects software VNC. Cocoa's GL path can crop the image on Retina displays, so QEngine uses VNC for GL.

To select a different build explicitly, pass `QEMU_BIN` in the environment. A persistent value in `instance.env` must be exported:

```sh
export QEMU_BIN=/absolute/path/to/prefix/bin/qemu-system-aarch64
DISPLAY_MODE=egl-vnc
```

Creation and rebuild commands rewrite `instance.env`; save custom settings before running them. See [Instances](../scripts/build_scripts/INSTANCES.md).

## Checks
The build finishes by checking linked graphics libraries, `virtio-gpu-gl-pci` and `egl-headless` support. Rerun those checks with:

```sh
scripts/build_scripts/build_virgl_qemu_macos.sh --stage test
```

The guest also needs virgl-capable Mesa. QEngine's rootfs builders install it for supported firmware; see [Display and input](engine/DISPLAY.md). If QEMU lacks the requested GL backend, the launcher warns and falls back to software. A rendered interface alone does not establish GPU rendering; guest renderer diagnostics should identify virgl rather than llvmpipe or softpipe.

## Stages
Use `--stage NAME` to rerun one part of the build, after its dependencies exist.

| Stage | Work |
| --- | --- |
| `deps` | Checks host tools, libraries and Metal compiler |
| `fetch` | Fetches pinned source revisions |
| `angle` | Builds ANGLE |
| `epoxy` | Builds EGL-capable libepoxy |
| `virgl` | Builds virglrenderer |
| `qemu` | Builds QEMU, `aarch64-softmmu` by default |
| `test` | Checks the resulting executable |
| `env` | Prints paths, settings and stage fingerprints |
| `all` | Runs dependency checks, fetch, builds and checks; the default |

`--targets LIST` changes QEMU's target list. The default covers QEngine's ARM guests. Source pins and build options live in [build_virgl_qemu_macos.sh](../scripts/build_scripts/build_virgl_qemu_macos.sh); [patches](../scripts/build_scripts/patches/README.md) documents the carried changes and their licenses.
