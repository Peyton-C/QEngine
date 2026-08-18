# dtshim

Serves a fake devicetree, and a usable `/proc/interrupts`, to an emulated Engine guest.
QEMU's `virt` machine synthesizes its own devicetree with none of the `inmusic,*`
properties Engine needs, so without this Engine reads an empty product code and aborts.

One source, two SoCs: build with `-DSOC_RK3588` or `-DSOC_RK3288`, which selects the
`DT_REMAPS` table of properties served and the `FALLBACK_INTERRUPTS` table below. It
refuses to compile without one. Everything else — the
`open`/`open64`/`fopen`/`fopen64`, `access`/`faccessat` and `write` interposition, the
runtime `/proc/interrupts` generation, the `/dev/mem` neutering — is shared.

`drmatomic.so` is required alongside this shim for `eglfs` to produce visible output;
without it Engine starts fine but the screen stays black. See BUILDING.md's arm64/RK3588
section for why. Both are installed and preloaded by the rootfs builders.

## What comes from where

**Compiled in, per SoC** (`DT_REMAPS` entries with a blob, served from an anonymous
`memfd` so nothing lands on the read-only rootfs):

- `rotation` — raw big-endian `<u32>`, served as 0, deliberately unlike the hardware.
  RK3588 answers four panel node paths and RK3288 one; see the audit below for why that
  covers every device and why 0 rather than the real value.
- `inmusic,az01-pcb-rev` (`B`) and `chosen/inmusic,internal-sd-fitted` — RK3288 only.
  No devicetree declares `pcb-rev` at all; it is u-boot-injected on `az01` boards, and
  `az04` has neither the pins nor the property. Again, see the audit.
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

## Checked against the real devicetrees

Audited against 55 shipped DTS files spanning all three board families — RK3288 `az01`,
RK3288 `az05`, and RK3588 `az04`/`az04b`. Conclusions, so nobody has to redo it:

**Panel coverage is complete.** Every RK3288 device — roughly forty across `az01` and
`az05` — puts `rotation` under `mipi@ff960000/panel@0`, so the single RK3288 entry
covers all of them. Every RK3588 device puts it under `dsi@fde20000/panel@0`. The four
display-node prefixes that appear anywhere in those files (`dsi@fde20000`,
`dsi@fde30000`, `edp-panel`, `mipi@ff960000`) are exactly the four the RK3588 table
serves. Of its extra three: `dsi@fde30000` and `edp-panel` exist because the JP22
`ScreenConfiguration` the arm64 builder writes points Qt at them, and `mipi@ff960000` is
the stale RK3288-lineage probe mentioned above. JP22 itself declares no `rotation`
property at all, so answering those paths with 0 is the whole of what it needs.

**We serve 0, and the hardware does not.** Real values are `<0xb4>` (180°) on RMZ2 and
`<0x10e>` (270°) on the RK3288 boards. That divergence is deliberate: the physical panels
are mounted rotated and the emulated virtio-gpu framebuffer is not, so serving the real
value would rotate the guest's output wrongly.

**`serial-number` and `inmusic,az01-pcb-rev` are in no devicetree.** Not one of the 55
declares either as a property. What `az01` boards do have is a pinctrl node *named*
`az01-pcb-rev` (`rockchip,pins`) — GPIO lines for reading the board revision — so u-boot
must read those pins at boot and inject the property. That is why both are synthesized
here rather than copied from a DTS, and why the RK3588 table correctly omits `pcb-rev`:
`az04` has no such pins and no such property, so a real RMZ2's Engine fails that read
too. Both Engine binaries do reference `inmusic,az01-pcb-rev`; it is shared code that
only `az01` hardware satisfies.

**`inmusic,panel-rotation` is not ours to serve.** It appears in 49 files, alongside
`rotation` and with the same value, and is read by the kernel's panel driver. Neither
Engine binary references it. Engine reaches `rotation` through the
`ScreenConfiguration` JSON rather than by a compiled-in path, which is why grepping the
binary for it finds nothing on arm64.

**`inmusic,internal-sd-fitted`** is declared by ten `az01`/`az05` files but referenced by
neither Engine binary. Served anyway on RK3288: it costs four bytes, and something
outside Engine may read it.

**Cross-checked against compiled dtbs, and against MPC.** The above is from DTS sources;
the 14 dtbs in `/boot` of a shipped ACV5 3.9.1 MPC rootfs agree. All 14 carry
`inmusic,product-code`, none contains `serial-number`, and the MPC update image ships no
bootloader at all — only a rootfs partition — so the thing that creates that node is not
in the image to be found. Nothing in the rootfs writes it either. That makes the
u-boot-injection explanation the only one left standing, and means there is no "real"
serial to match: on an emulated guest the node exists only because something fabricates
it, which here is this shim.

Those dtbs also line up with the RK3288 table property for property — `rotation` under
`mipi@ff960000/panel@0` in 14/14, `inmusic,internal-sd-fitted` in 10, and the
`az01-pcb-rev` pinctrl node in 11 (the three `az05` dtbs lack it, exactly as the source
audit predicted). So an RK3288 dtshim would serve an MPC guest correctly as-is. Nothing
is what the MPC build now does, with this shim unmodified — see BUILD_MPC.md. MPC is
therefore a third consumer, and the first that wants only the identity half: it reads
`inmusic,product-code` and `serial-number` through `libaz0x-info` and nothing else the
table serves.

## Existence checks, not just opens

`access` and `faccessat` are interposed alongside the open family, because a caller
may check before it reads and never open at all. MPC does exactly that, and it is
why an emulated MPC identified itself as `<Unknown>` for a while despite the shim
serving `inmusic,product-code` correctly to anything that opened it. Measured in the
guest, with the shim preloaded:

```
LD_PRELOAD=/root/dtshim.so sh -c '[ -r …/inmusic,product-code ]'   -> failed
LD_PRELOAD=/root/dtshim.so cat …/inmusic,product-code              -> ACV5
```

MPC imports `access` and — checked against its dynamic symbols — no member of the
stat family at all, so the guard failed on the real sysfs path and the read was
abandoned. Engine never showed this because it opens directly.

A blob-backed property is answered from the table rather than from a file, since
there is no file anywhere to consult: readable yes, writable or executable no.
Remapped properties defer to the real call against the remapped path, so a missing
`/root/fake-dt` still reports missing.

The stat family is deliberately **not** interposed. Nothing needs it — MPC imports
none of it, Engine opens directly — and it is the riskiest family to get wrong:
`stat`/`stat64`/`__xstat`/`__xstat64`/`statx` vary by glibc version and by
`_FILE_OFFSET_BITS`, so a mistake there breaks every caller rather than only the
ones that wanted a devicetree. Add it when something demonstrably needs it.

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
