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
# --device is optional; the family is identified from the firmware when omitted
scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 \
    --firmware ~/firmware/SYSTEMONE-5.0.4-Update.img

scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --device mpc \
    --firmware ~/firmware/MPC-3.9.1-Gen1-update.img
```

That is the whole setup — the kernel is built on demand, once per architecture, and
reused by every later instance that needs it.

`--device` selects the device family, which decides the shim stack and the disk
layout: `engine` (Engine OS) or `mpc` (Akai MPC). Add `--force` to rebuild an
existing rootfs, `--size` to change the image size.

### Device families are a registry, and the choice is checked

The families live in one table in `new_instance.sh`, a row per family/architecture
combination:

```
<family>|<arch>|<rootfs markers>|<rootfs builder>|<disk layout>|<data image name>
engine|arm64|/usr/Engine|build_arm64_rootfs.sh|engine|data_disk.img
engine|armhf|/usr/Engine|build_armv7_engine_rootfs.sh|mpc|emmc.img
mpc|armhf|/usr/bin/MPC|build_mpc_rootfs.sh|mpc|emmc.img
```

The markers are paths that must **all** exist in the built rootfs for the row to
match, so a family needing more than one piece of evidence lists more. Rows are
tried in order, letting a family whose markers are a superset of another's be listed
first. Adding a device family, or a new architecture for one, is adding a row.

Architecture is part of the key because two things differ along it rather than along
the family. Engine OS on armv7 needs its own rootfs builder — a different shim stack,
and RK3288 devicetree paths — and it wants the single-partition `mpc` disk layout
rather than the RK3588 `data`+`factory` pair, because its `data.mount` asks for the
`az01-internal` PARTUUID. Disk layout tracks the platform generation, not the
application. The markers themselves say nothing about architecture (the same
`/usr/Engine` tree ships on both), so that comes from the dynamic loader's name.

`--device` is optional. It narrows the builder down to a family, and that much can be
decided from the firmware instead, so when it is omitted the family is resolved in
order of cost: what was asked for, then what this instance was built as before (free,
its rootfs is already on disk), then the firmware itself, which costs one extraction
of a few seconds.

Passing `--device` skips that extraction only when the family pins a single row —
`mpc` today. `--device engine` still has to look inside the firmware, because Engine
OS ships on both architectures and they take different builders. Either way, stating
the family records the intent in `instance.env` and in shell history.

Either way it is verified rather than trusted: once the rootfs exists it is identified
from its markers, and a disagreement is a hard error before the data disk is created.

That check earns its keep on a mismatch the architecture guards cannot see. Those
compare a rootfs against their own builder's architecture, so engine-versus-mpc
confusion is only caught while the two families differ in architecture. An arm64 MPC
image built as `--device engine` passes them and gets the entire Engine shim stack
installed into an MPC rootfs — an instance that boots and can never work.

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
| machine | `virt,highmem=on` | `virt,highmem=off` |
| GPU | `virtio-gpu-pci` | `virtio-gpu-pci` |
| input | `usb-ehci`/`qemu-xhci` + `usb-kbd`/`usb-tablet` | same |
| net | `virtio-net-pci` | `virtio-net-pci` |
| audio | `ich9-intel-hda` + `hda-output` | same |
| RAM default | 4096 | 2048, and 3072 is a hard ceiling |
| CPU / accel | `max` (`host` under HVF/KVM) | `cortex-a15`, always TCG |

Only the first two rows really differ, and the second follows from the first. armhf
used to attach everything over virtio-mmio because the Debian armmp kernel could not
probe that machine's PCI host bridge (`pci-host-generic ... failed with error -75`),
which made every `-pci` device invisible, USB controllers included. That error is
`-EOVERFLOW`, and it is a memory-map problem rather than a missing driver: with
`highmem=on` the PCIe ECAM sits at `0x40_10000000`, beyond what the non-LPAE armmp
kernel's 32-bit `resource_size_t` can hold. `highmem=off` moves it to `0x3f000000`
and the bus enumerates normally, so both architectures now use the same devices.

The cost is that the guest's physical address space stops at 4GB, and since RAM
starts at `0x40000000` that leaves 3072 as the most armhf can be given — past it
QEMU refuses to start outright, so `arch_devices.sh` checks `MEM` and says why. If an
armhf guest ever needs more, the alternative is `linux-image-armmp-lpae`, which has
`CONFIG_PHYS_ADDR_T_64BIT=y` and can therefore keep `highmem=on`.

Two things are still arm64-only in practice. The virgl display modes refuse armhf —
`virtio-gpu-gl-pci` is attachable now, but virgl against a 32-bit guest is untested,
so the GL modes fail up front instead of partway through boot. And `GPU_MAX_OUTPUTS`
no longer *rejects* armhf, since per-head `usb-tablet`s need the USB that needed PCI,
but multi-head has only been run on arm64.

One binary serves both — `qemu-system-aarch64` offers `cortex-a15`/`cortex-a7` and
boots a 32-bit zImage, verified against the armv7 MPC rootfs. `qemu-system-arm` is
used only as a fallback where the 64-bit build is not installed, and `QEMU_BIN`
overrides the choice.

armv7 Engine is no longer one of those: `build_armv7_engine_rootfs.sh` produces one,
and it boots to a rendered UI with a virtual control surface bound. Audio is what it
still lacks. arm64
MPC remains untried — no builder produces that rootfs, so the command line has never
been booted, and `new_instance.sh` says so when it sees one. The virgl display modes
additionally refuse armhf outright, since `virtio-gpu-gl` exists only as a PCI device,
which is also why armv7 Engine renders in software (`kms_swrast`).

## Run

```sh
scripts/qemu/run_instance.sh --list
scripts/qemu/run_instance.sh --name mpc-3.9.1
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display vnc
```

`--display` overrides the `DISPLAY_MODE` recorded in `instance.env` for one run. The
modes are `sdl`, `sdl-gl`, `cocoa`, `vnc`, `egl-vnc` and `none`; see
[../qemu/display_modes.sh](../qemu/display_modes.sh).

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
| `DEVICE` | the device family, detected from the rootfs (see above) |
| `DISPLAY_MODE` | which display backend to boot with; defaults to the host's (`cocoa` on macOS, `sdl` elsewhere) |
| `ROOTFS_IMG`, `DATA_IMG` | this instance's disks |
| `ROOT_UUID` | read off the built image; it differs per firmware version, which is why it is never hardcoded |
| `ARCH` | `arm64` or `armhf`, detected from the rootfs (see above) |
| `KERNEL_IMG`, `INITRD_IMG` | the kernel that matches `ARCH`, shared by every instance of it |
| `SSH_PORT`, `VNC_DISPLAY` | derived from the instance name so two instances cannot collide. Edit if a port is already taken on the host |

`run_instance.sh` exports these and execs `run_qemu.sh`, which resolves the machine
type, devices, kernel, accelerator, audio and display from `ARCH`, `DEVICE` and
`DISPLAY_MODE`. There are no per-device or per-display launcher scripts: they were
seven near-copies of one command line, and every value that distinguished them now
lives in `instance.env` where it can be read and edited.

`run_qemu.sh` falls back to the paths and ports those launchers used to hardcode, so it
can also be run directly against a hand-built `build/rootfs_out.img`.

## Notes

- Booting the same instance twice is refused via a lock file. `flock` is
  Linux-only, so on macOS QEMU's own image locking is the only guard.
- `data_disk.img` / `emmc.img` hold the guest's own state and survive `--force`.
  Delete one by hand for a factory-fresh guest.
- See [BUILD_ARM64.md](BUILD_ARM64.md) and [BUILD_MPC.md](BUILD_MPC.md) for what
  each device family needs beyond this.
