# dtshim
`dtshim` serves the devicetree, a usable `/proc/interrupts` and a few hardware sysfs files to an emulated Engine guest. QEMU's `virt` machine builds its own devicetree with none of the `inmusic,*` properties, so without the shim Engine reads an empty product code and aborts. MPC, MPC Sample, HeadRush and the Alesis drum modules preload it too, for the product code and serial.

One source covers every SoC. Build with `-DSOC_RK3588`, `-DSOC_RK3566` or `-DSOC_RK3288`, which picks the properties served and the fallback interrupt table; it refuses to compile without one. The rootfs builders build and preload it.

`drmatomic.so` must be preloaded beside it for `eglfs` to show anything; without it Engine starts but the screen stays black. See [Display and input](../../docs/engine/DISPLAY.md).

## Served paths
| Path | SoC | Served from |
| --- | --- | --- |
| `inmusic,product-code` | all | `/root/fake-dt`, written per instance |
| `serial-number` | all | `/root/fake-dt`, written per instance |
| `*/panel@0/rotation` under `dsi@fde20000`, `dsi@fde30000`, `mipi@ff960000`, and `edp-panel/rotation` | RK3588 | `0` |
| `inmusic,az07-pcb-rev` | RK3566 | `A` |
| `spi@fe640000/display@0/rotation`, `inmusic,panel-rotation` | RK3566 | `0` |
| `inmusic,az01-pcb-rev` | RK3288 | `B` |
| `mipi@ff960000/panel@0/rotation` | RK3288 | `0` |
| `chosen/inmusic,internal-sd-fitted` | RK3288 | `0` |
| `chosen/inmusic,emmc-drum-content` | RK3288 | `/root/fake-dt`, written for `LDMF` only |
| `/sys/devices/platform/ffb10000.efuse`, `ffb40000.efuse` | RK3288 | `/root/fake-dt`, eFuse fixtures |
| `/sys/class/power_supply/sbs-3-000b`, `az01-ac-power` | RK3288 | `/root/fake-power`, see Battery |
| `/dev/mem` | all | `/root/fake-dev-mem`, sparse and zero-filled |
| `/proc/interrupts` | all | generated at runtime |

Devicetree paths are relative to `/sys/firmware/devicetree/base`. Fixed values are served from memory, so nothing lands on the read-only rootfs. A path served from a file must have that file in the guest, or the read fails with `ENOENT`, so the builders write exactly this set through `rootfs_steps/write_fake_dt.sh`.

Rotation is served as 0, unlike the hardware. The real panels are mounted rotated and QEMU's framebuffer is not, so the real value would turn the picture. `serial-number` and the `pcb-rev` properties appear in no shipped devicetree, since u-boot injects them, so `dtshim` makes them up. RK3588 serves no `pcb-rev`, and a real RMZ2's Engine fails that read too.

`/dev/mem` reads zeros, so the anti-clone check that probes physical memory finds nothing. The file is 4400 MiB so that RK3288 register space falls inside it, because a mapping past the end of a file succeeds and then faults with `SIGBUS`.

## Interposition
`dtshim` interposes the `open` and `fopen` families, `access` and `faccessat`, and `write`. Existence checks matter because MPC tests a path with `access` and never opens it if that fails. A property served from memory reports readable and nothing else. The stat family is left alone, except `statx` and the time64 calls on RK3288 for the sysfs redirects, because a mistake there breaks every caller.

## Interrupts
Engine aborts unless `/proc/interrupts` names IRQs for `dwc3`, `fe210000.sata`, `fea10000.dma-controller`, `ff0c0000.dwmmc`, `ff0f0000.dwmmc` and `ttyS0`, then sets CPU affinity on each. `dtshim` reads the real file on first open, picks six MSI or edge IRQs that accept an affinity write, and relabels them. Engine sets affinity through `sh -c`, so the shim passes the mapping to the child in `DTSHIM_FAKE_IRQS` and fakes success for affinity writes to those six IRQs.

`FALLBACK_INTERRUPTS`, compiled in per SoC, is used only when the real file cannot be read or has no usable IRQ. To regenerate it, boot the guest without the shim, read `/proc/interrupts`, and keep six IRQs that accept a write:

```sh
for i in <candidate IRQ numbers>; do
  printf 'IRQ %s: ' "$i"
  echo 1 > /proc/irq/$i/smp_affinity 2>&1 && echo OK || echo FAIL
done
```

## Logging
On RK3588 builds, `DTSHIM_DT_LOG=1` logs every devicetree open to `/root/dtshim-dt-access.log`, with the path asked for and what served it. It slows Engine down, so leave it off otherwise.

## RK3566
MPC Sample 1.3.0 uses `-DSOC_RK3566`. QEngine gives it a landscape virtual display in place of the rotated SPI panel, which is why rotation is 0.

## Legacy Engine
SC5000 1.2.0, 1.2.2 and 1.3.1 and Prime 4 1.3.2, 1.3.3 and 1.4.1 use the same source with `legacy_compat.c`, linked against the guest's glibc. It supplies `memfd_create` through the kernel, covers fortified open calls and serves physical-register mappings as anonymous zero-filled memory. The builder enables large-file access for `/dev/mem`'s sparse stand-in. Newer firmware builds omit this compatibility object.

## Battery
`dtshim` redirects these RK3288 power-supply paths to `/root/fake-power`. `write_fake_dt` creates the files for `JP11`, `JP11S` and `NH10`, keeping Engine on AC with a full battery.

| Sysfs path | Value |
| --- | --- |
| `/sys/class/power_supply/sbs-3-000b/present` | `1` |
| `/sys/class/power_supply/sbs-3-000b/capacity` | `100` |
| `/sys/class/power_supply/sbs-3-000b/status` | `Full` |
| `/sys/class/power_supply/az01-ac-power/online` | `1` |

Rebuild existing instances to install both the shim and the files. See [Identity](../../docs/engine/IDENTITY.md#battery-identities) for firmware coverage.
