# Identity
Engine reads `/sys/firmware/devicetree/base/inmusic,product-code` to select its product configuration. That code controls the interface, screen assumptions, audio-device names and hardware behavior. QEMU's `virt` devicetree has no inMusic identity, so `dtshim` serves the properties Engine expects.

## Products
Select an identity with `new_instance.sh --product-code CODE`. QEngine records it in `instance.env` so `--force` keeps it. A firmware image must contain support for the chosen identity; a directory bearing another product's name does not establish that support.

| Code | Device |
| --- | --- |
| `JC16` | Denon DJ Prime 2 |
| `JC11` | Denon DJ Prime 4 |
| `JC11S` | Denon DJ Prime 4+ |
| `JP24` | Denon DJ Prime 4 G2 |
| `JP11` | Denon DJ Prime GO |
| `JP11S` | Denon DJ Prime GO+ |
| `JP07` | Denon DJ SC5000 Prime |
| `JP08` | Denon DJ SC5000M Prime |
| `JP13` | Denon DJ SC6000 Prime |
| `JP14` | Denon DJ SC6000M Prime |
| `JP20` | Denon DJ SC Live 2 |
| `JP21` | Denon DJ SC Live 4 |
| `NH08` | Numark Mixstream Pro |
| `NH08S` | Numark Mixstream Pro+ |
| `NH10` | Numark Mixstream Pro GO |
| `RMZ2` | RANE SYSTEM ONE |

The [device tables](../../README.md#emulated-controllers) list the firmware versions booted under each identity.

## Devicetree
| Property or interface | QEngine handling |
| --- | --- |
| `inmusic,product-code` | Reads the instance's `/root/fake-dt/inmusic,product-code` |
| `serial-number` | Reads the instance's `/root/fake-dt/serial-number` |
| Panel `rotation` | Serves a big-endian u32 zero for the virtual display |
| `inmusic,az01-pcb-rev` | Serves `B` on RK3288 |
| `chosen/inmusic,internal-sd-fitted` | Supplies the RK3288 storage property |
| `/dev/mem` | Supplies a sparse stand-in or anonymous mappings rather than physical registers |
| `/proc/interrupts` | Supplies the component names Engine expects and handles affinity writes for the mapped IRQs |

Write identity strings without a trailing newline. Devicetree cells are binary, so writing text `0` is not a valid rotation cell. QEngine's builders handle both formats.

`dtshim` chooses usable IRQs at runtime, since their numbers change with QEMU's device list. It also handles the affinity writes made by Engine's shell children. See [dtshim](../../shims/dtshim/README.md) for the implementation and SoC-specific paths.

## Hardware checks
Signed RK3288 firmware checks Rockchip eFuse identity. QEngine supplies it through the shared devicetree step. RK3588 Engine 5.1.0 and later also checks TEE attestation; `teeshim` answers it so Engine does not exit to TestApp. These shims leave the application binary unmodified.

## Battery identities
Prime GO can require a hold gesture at startup and quit without it. Building as `JC11` avoids that gate in several releases, but Prime GO 1.5.2 keeps the check in its product-specific binary. Native `JP11` rendering has also shown only the upper-left part of the interface. QEngine has no verified general fake-AC setting.

On 5.3.0, Prime GO+ as `JP11S` and Mixstream Pro GO as `NH10` started Engine, showed a blank screen and powered off after about two minutes. Their images booted as `JC11S` and `NH08S`. Use those alternate identities when the native one will not stay up, accepting the alternate product's interface and control map.
