# dtshim

Serves a fake devicetree, and a usable `/proc/interrupts`, to an emulated Engine guest.
QEMU's `virt` machine synthesizes its own devicetree with none of the `inmusic,*`
properties Engine needs, so without this Engine reads an empty product code and aborts.

One source, two SoCs: build with `-DSOC_RK3588` or `-DSOC_RK3288`, which selects the
`DT_REMAPS` table of properties served and the `FALLBACK_INTERRUPTS` table below. It
refuses to compile without one. Everything else — the `open`/`open64`/`fopen`/`fopen64`
and `write` interposition, the runtime `/proc/interrupts` generation, the `/dev/mem`
neutering — is shared.

`drmatomic.so` is required alongside this shim for `eglfs` to produce visible output;
without it Engine starts fine but the screen stays black. See BUILDING.md's arm64/RK3588
section for why. Both are installed and preloaded by the rootfs builders.

## What comes from where

**Compiled in, per SoC** (`DT_REMAPS` entries with a blob, served from an anonymous
`memfd` so nothing lands on the read-only rootfs):

- `rotation` — raw big-endian `<u32>`, served as 0. The real RMZ2 `system.dtb` has 180
  (`\x00\x00\x00\xb4`) on `dsi@fde20000/panel@0/rotation`, but 0 is what the emulated
  panel wants and what has been confirmed working. RK3588 answers four different panel
  node paths with the same value, because which node exists depends on the display the
  image is built for and Engine probes whichever it expects.
- `inmusic,az01-pcb-rev` (`B`) and `chosen/inmusic,internal-sd-fitted` — RK3288 only;
  RMZ2's devicetree has no such properties.
- `FALLBACK_INTERRUPTS` — see below.

**Written by the builder**, via `scripts/build_scripts/rootfs_steps/write_fake_dt.sh`:

- `inmusic,product-code` and `serial-number`. These are identity rather than hardware
  description: both are worth setting per instance, Engine shows the serial in Settings
  as `DeviceSerialNumber`, and `midisurface` opens the product-code file directly to
  decide which device to answer Engine's inquiry as. `write_fake_dt` takes the code and
  an optional serial, and writes them with `printf` rather than copying a fixture —
  these must have no trailing newline, which a repo file easily picks up and which
  Engine's direct `fopen()`/`read()` callers will not tolerate.
- `/root/fake-dev-mem`, an empty file `/dev/mem` remaps to, so an `mmap` of it fails
  cleanly rather than handing out real physical memory. That is what the hardware
  anti-clone check probes.

## `/proc/interrupts`, and why it is generated rather than stored

Engine hard-throws (`std::runtime_error`, aborts) if it cannot find IRQ lines for
`dwc3`, `fe210000.sata`, `fea10000.dma-controller`, `ff0c0000.dwmmc`, `ff0f0000.dwmmc`
and `ttyS0` by name — none of which exist under QEMU's `virt` machine — and it
immediately writes CPU affinity to whatever `/proc/irq/<N>/` number it found each one
at. So a static table needs real, currently-writable IRQ numbers that stay valid.

**IRQ/MSI vector numbers are not stable across QEMU device-list changes.** They are
assigned at boot from exactly which devices are present and in what order, and not
every real IRQ accepts an `smp_affinity` write either — GPIO-backed and per-CPU IRQs
like `arm-pmu` return `EIO` despite passing a permission check. That used to mean
regenerating a file by hand after every device-list change.

`build_fake_interrupts()` does it at runtime instead: on the first `/proc/interrupts`
open per process it reads the *real* file, filters to `MSI`/`Edge` lines (matching both
plain `MSI` and `ITS-MSI` — the controller label varies by kernel build, confirmed
directly when switching from the Ubuntu cloud kernel to Debian trixie's, see
[get_kernel.sh](../../scripts/build_scripts/get_kernel.sh)), verifies each candidate
with a real no-op read-then-write-back probe rather than `access(W_OK)` — that is what
the `arm-pmu`-style `EIO` case slips past — then relabels six of them with the names
Engine wants. Picked once and cached per process, so lookups stay consistent within a
boot.

That probe alone is not sufficient, also confirmed directly: a candidate can pass at
read time and still fail Engine's affinity write moments later with `EPERM`, because
Engine does not set affinity itself — it shells out
(`sh -c "echo ... > .../smp_affinity_list"`) — and the real MSI vector our fake name
landed on can go from freely-reaffinitizable to pinned once whatever real virtio device
owns it finishes its own init, on its own schedule. So the shim also intercepts
`write()` and fakes success for `smp_affinity`/`smp_affinity_list` writes targeting the
specific real IRQ numbers it mapped, propagated to the shelled-out child through
`DTSHIM_FAKE_IRQS` the same way `LD_PRELOAD` reaches that child — the child is a fresh
`exec` of `/bin/sh`, not a fork continuing Engine's memory, so it never reads
`/proc/interrupts` itself and needs the mapping handed to it.

`FALLBACK_INTERRUPTS` is only consulted if that path fails outright — the real
`/proc/interrupts` cannot be read at all, or no `MSI`/`Edge` candidate exists. At that
point it is exactly as stale and fragile as a stored file ever was. It lives in the
shim rather than on disk so it cannot drift from the parser above it, or be forgotten by
a builder.

If it ever needs regenerating: boot the guest, read the real `/proc/interrupts` with the
shim not loaded to see the current numbering, then confirm which candidates actually
accept a write before reusing them:

```sh
for i in <candidate IRQ numbers>; do
  printf 'IRQ %s: ' "$i"
  echo 1 > /proc/irq/$i/smp_affinity 2>&1 && echo OK || echo FAIL
done
```

Only reuse `OK` IRQs, one real IRQ per fake name, six needed in total.

## Devicetree access logging

`DTSHIM_DT_LOG=1` logs every devicetree open to `/root/dtshim-dt-access.log`, with the
path Engine asked for and what it was served from. RK3588 builds only — it was written
for that bring-up, and it is not free to leave on: every matching access takes a mutex
and does a separate `fopen`/`fprintf`/`fclose`, and `serial-number` alone is re-read
dozens of times per session. Two full passes with it on found Engine reading only
`inmusic,product-code`, `serial-number` and one stale RK3288-era rotation probe, which
is what ruled the devicetree out of the audio investigation.
