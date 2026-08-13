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
# once per architecture — the kernel is generic and shared by every instance
scripts/build_scripts/get_arm64_kernel.sh      # for engine instances
scripts/build_scripts/get_armv7_kernel.sh      # for mpc instances

scripts/build_scripts/new_instance.sh --name rmz2-5.0.4 --device engine \
    --firmware ~/firmware/SYSTEMONE-5.0.4-Update.img

scripts/build_scripts/new_instance.sh --name mpc-3.9.1 --device mpc \
    --firmware ~/firmware/MPC-3.9.1-Gen1-update.img
```

`--device` selects the family: `engine` (arm64 / RK3588 Engine OS) or `mpc`
(armv7 / RK3288 Akai MPC). Add `--force` to rebuild an existing rootfs, `--size`
to change the image size.

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
distro kernels, byte-identical for every instance of the same architecture.

## instance.env

Generated once, then yours to edit — nothing regenerates it unless you delete it:

| Key | Meaning |
|---|---|
| `LAUNCHER` | which `scripts/qemu/` script to exec |
| `ROOTFS_IMG`, `DATA_IMG` | this instance's disks |
| `ROOT_UUID` | read off the built image; it differs per firmware version, which is why it is never hardcoded |
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
