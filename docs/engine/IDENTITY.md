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
QEngine supplies a full battery on AC for `JP11`, `JP11S` and `NH10`. `dtshim` redirects Engine's battery and AC sysfs reads to files written by `write_fake_dt`. Build with the native product code; existing instances need a rebuild to install the shim and files.

| Device | Product code | Native UI boot |
| --- | --- | --- |
| Prime GO | `JP11` | 5.3.0, armv7 |
| Prime GO+ | `JP11S` | 5.3.0, armv7 |
| Mixstream Pro GO | `NH10` | 5.3.0, armv7 |

Older releases have not been booted with the fake battery. The earlier Prime GO boots marked `!` used `JC11`; 1.5.2 still quit on its battery check with that identity. Audio and the full control map remain unverified on these three native identities.
