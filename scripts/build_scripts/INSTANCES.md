# Instances — running several emulated devices side by side

One directory per emulated device, in the spirit of an Android AVD. Each instance
owns its disks and its configuration, so building or booting one never disturbs
another.

Without this, every rootfs build writes `build/rootfs_out.img` and every launcher
opens that same path on port 2225: a second device silently overwrites the first,
and a second VM either fails to bind the port or — worse — two QEMUs write the
same disk, which corrupts it without an error.

## Create

```sh
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 --device engine \
    --firmware ~/firmware/SYSTEMONE-5.0.4-Update.img

scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --device mpc \
    --firmware ~/firmware/MPC-3.9.1-Gen1-update.img
```

That is the whole setup — the kernel is built on demand, once per architecture, and
reused by every later instance that needs it.

`--device` selects the device family, which decides the shim stack and the disk
layout: `engine` (Engine OS) or `mpc` (Akai MPC). Add `--force` to rebuild an
existing rootfs, `--size` to change the image size.

### Architecture is detected, not assumed

`--device` deliberately does **not** imply the architecture. Engine OS ships on both
RK3288 (armv7) and RK3588 (arm64), and so does MPC, and the container format does not
settle it either — the `AZ0x` images span both. So the architecture is read off the
built rootfs by looking for its dynamic loader (`ld-linux-aarch64.so.1` vs
`ld-linux-armhf.so.3`), which is unambiguous and needs no product-code table to keep
up to date.

This works because the kernel is not needed until boot: by the time one has to be
chosen, the filesystem that decides it has already been built. The result is recorded
as `ARCH` in `instance.env` along with the kernel paths it selected.

The launchers follow that result rather than their own name. `arch_devices.sh`,
sourced by each of them, resolves the QEMU binary and the architecture-dependent
devices from `ARCH`:

| | arm64 | armhf |
|---|---|---|
| machine | `virt,highmem=on` | `virt` + `virtio-mmio.force-legacy=false` |
| GPU | `virtio-gpu-pci` | `virtio-gpu-device` |
| input | `usb-ehci`/`qemu-xhci` + `usb-kbd`/`usb-tablet` | `virtio-keyboard-device`/`virtio-tablet-device` |
| net | `virtio-net-pci` | `virtio-net-device` |
| audio | `ich9-intel-hda` + `hda-output` | `virtio-sound-device` |

The 32-bit split is forced: the Debian armmp kernel cannot probe that machine's PCI
host bridge (`pci-host-generic ... failed with error -75`), so every `-pci` device is
invisible there, USB controllers included.

One binary serves both — `qemu-system-aarch64` offers `cortex-a15`/`cortex-a7` and
boots a 32-bit zImage, verified against the armv7 MPC rootfs. `qemu-system-arm` is
used only as a fallback where the 64-bit build is not installed, and `QEMU_BIN`
overrides the choice.

What is still untried is the *combination*: no builder produces an armv7 Engine or an
arm64 MPC rootfs, so those command lines have never been booted. `new_instance.sh`
says so when it sees one. The virgl launchers additionally refuse armhf outright,
since `virtio-gpu-gl` exists only as a PCI device.

## Run

```sh
scripts/qemu/run_instance.sh --list
scripts/qemu/run_instance.sh --name mpc-3.9.1
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --launcher systemone_vnc.sh
```

`--launcher` overrides the launcher recorded in `instance.env`, which is how you
pick a different display backend (`systemone_vnc.sh`, `systemone_linux_virgl.sh`,
`mpc_macos.sh`, ...).

## Layout

```
build/
  vmlinuz-generic-arm64, initrd-generic-arm64    shared by every engine instance
  vmlinuz-generic-armhf, initrd-generic-armhf    shared by every mpc instance
  instances/
    rmz2-5.0.4/{rootfs.img, data_disk.img, instance.env}
    mpc-3.9.1/{rootfs.img, emmc.img, instance.env}
```

The kernel and initrd are deliberately **not** per-instance: they are generic
distro kernels, byte-identical for every instance of the same architecture. Building
one is a no-op once it exists, so creating a second instance of the same
architecture skips straight to the rootfs.

## instance.env

Generated once, then yours to edit — nothing regenerates it unless you delete it:

| Key | Meaning |
|---|---|
| `LAUNCHER` | which `scripts/qemu/` script to exec |
| `ROOTFS_IMG`, `DATA_IMG` | this instance's disks |
| `ROOT_UUID` | read off the built image; it differs per firmware version, which is why it is never hardcoded |
| `ARCH` | `arm64` or `armhf`, detected from the rootfs (see above) |
| `KERNEL_IMG`, `INITRD_IMG` | the kernel that matches `ARCH`, shared by every instance of it |
| `SSH_PORT`, `VNC_DISPLAY` | derived from the instance name so two instances cannot collide. Edit if a port is already taken on the host |

`run_instance.sh` exports these and execs the ordinary launcher, so the QEMU
command lines live in one place each. Those launchers fall back to the old
hardcoded paths and ports when the variables are unset, so running them directly
still works exactly as before.

## Notes

- Booting the same instance twice is refused via a lock file. `flock` is
  Linux-only, so on macOS QEMU's own image locking is the only guard.
- `data_disk.img` / `emmc.img` hold the guest's own state and survive `--force`.
  Delete one by hand for a factory-fresh guest.
- See [BUILD_ARM64.md](BUILD_ARM64.md) and [BUILD_MPC.md](BUILD_MPC.md) for what
  each device family needs beyond this.
