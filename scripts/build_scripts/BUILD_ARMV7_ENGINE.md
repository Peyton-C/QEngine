# Setup everything required to emulate armv7 Engine OS
Requires docker, qemu, binwalk 3.1.X, e2fsprogs.

This is Engine OS on the RK3288 controllers (Prime, SC, Mixstream), as opposed to
[BUILD_ARM64.md](BUILD_ARM64.md), which is the same application on RK3588. The two
need different rootfs builders and different disk layouts; `new_instance.sh` picks
between them from the firmware itself.

The short way is one command, which does all of the below and keeps each device in its
own directory — see [INSTANCES.md](INSTANCES.md):

```sh
scripts/build_scripts/new_instance.sh --name jp07-5.0.4 --firmware PATH_TO_PRIME_UPDATE.img
scripts/qemu/run_instance.sh --name jp07-5.0.4
```

The steps individually, if you want them:

1. [Download Engine from InMusic](https://enginedj.com/downloads) for an RK3288 controller.
2. Build the rootfs with `build_armv7_engine_rootfs.sh --firmware PATH_TO_PRIME_UPDATE.img`.
3. Get the kernel and initrd with `get_kernel.sh --arch armhf`.
4. Make a /data disk with `make_disk.sh --family mpc` — see the note below on why it
   is not `--family engine`.
5. Boot with `DEVICE=engine ARCH=armhf scripts/qemu/run_qemu.sh`.
   `DISPLAY_MODE` picks the display backend (`sdl`, `cocoa`, `vnc`, `none` — but not
   `sdl-gl` or `egl-vnc`, see below).

## Notes

- **The /data disk uses the `mpc` layout, not the `engine` one.** This rootfs's
  `data.mount` asks for PARTUUID `931ad49d-ad59-0849-833a-9bf00af5b60e`, the single
  `az01-internal` partition, which is what the MPC images use — not the RK3588
  `data`+`factory` pair. Disk layout tracks the platform generation, not the
  application. `new_instance.sh` gets this right on its own.

- **One image, several products.** A single update image serves multiple device
  identities (`JP07-JP08-JP11-5.0.4.img` covers all three), and `/usr/Engine` is
  shared across them, so the devicetree product code is the only thing that
  distinguishes them. `PRODUCT_CODE=JP11 build_armv7_engine_rootfs.sh ...` picks
  one; the default is `JP07`. Only `JP07` has actually been booted.

- **Rendering is software, unavoidably.** `virtio-gpu-gl` is a PCI-only device and
  the 32-bit `virt` machine has no usable PCI, so virgl is off the table and the
  guest runs on `kms_swrast`. The `sdl-gl` and `egl-vnc` display modes refuse
  armhf outright for the same reason. Expect it to be slow under TCG.

- **Engine needs its EGL device integration named explicitly** —
  `QT_QPA_EGLFS_INTEGRATION=eglfs_kms`, plus `EGL_PLATFORM=gbm` and
  `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`. The rootfs build writes all three into
  `engine.service.d/override.conf`. Left to itself Qt picks no integration at all
  and Engine dies in an EGL restart loop; see
  [../../docs/BUILDING.md](../../docs/BUILDING.md#engine-504-on-armv7-rk3288).

- **The shims are shared with the arm64 build**, except `dtshim_jc11s.c`, which
  carries RK3288's devicetree paths. `drmatomic` and `touchbridge` build from the
  RK3588 sources. One 32-bit-specific catch is worth knowing before writing another
  shim here: this guest's glibc is a 64-bit-`time_t` build, so it imports
  `__ioctl_time64` rather than `ioctl`, and an `LD_PRELOAD` interposer has to export
  both names or it loads and silently never runs.

- Used directly as above, the rootfs is written to `build/rootfs_out.img` — the same
  path the arm64 and MPC builds use, so building one target overwrites the other.
  Use an instance (see [INSTANCES.md](INSTANCES.md)) to keep several side by side.

- **What is not done yet:** audio and the control surface. The arm64 build's
  `alsashim`, `midisurface` and `controllermap` are RMZ2-specific and were
  deliberately left out here, so the guest boots and renders but has no sound card
  Engine will accept.
