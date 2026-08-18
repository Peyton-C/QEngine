# fake-dt-rmz2

Fake devicetree/proc files for `dtshim_rmz2.so` (RANE SYSTEM ONE / RMZ2),
deployed to `/root/fake-dt/` in the guest. Confirmed working all the way
through Engine rendering its onboarding screen on-screen via `eglfs` — see
BUILDING.md's arm64/RK3588 section for the full story and current status.

- `inmusic,product-code` — deploy with `printf RMZ2 > ...`, **not** a plain
  file copy; these text files in the repo may pick up a trailing newline
  that direct `fopen()`/`read()` callers in Engine won't tolerate the way a
  shell `$(cat ...)` substitution (which strips trailing newlines) will.
- `serial-number` — arbitrary placeholder value, not derived from a real
  unit. Deploy the same way (`printf ... > ...`, no trailing newline).
- `rotation` — raw big-endian `<u32>` devicetree cell, not text. Build with
  `printf '\x00\x00\x00\xb4'` (180 decimal, from the real `system.dtb`'s
  `dsi@fde20000/panel@0/rotation` property) — not stored as a repo file
  here since it's binary.
- `interrupts` — **static fallback only**, as of the dynamic-generation
  change in `dtshim_rmz2.c`. Engine hard-throws (`std::runtime_error`,
  aborts) if it can't find IRQ lines for `dwc3`, `fe210000.sata`,
  `fea10000.dma-controller`, `ff0c0000.dwmmc`, `ff0f0000.dwmmc`, and
  `ttyS0` by name — none of which exist under QEMU's `virt` machine — and
  immediately writes CPU affinity to whatever `/proc/irq/<N>/` number it
  finds each one at, so a static file needs real, currently-writable IRQ
  numbers that stay valid.

  **IRQ/MSI-vector numbers are not stable across QEMU device-list
  changes** — they're assigned dynamically at boot based on exactly which
  devices are present and in what order, and not every real IRQ accepts an
  `smp_affinity` write either (GPIO-backed and per-CPU IRQs like `arm-pmu`
  return `EIO` despite passing a permission check). This used to mean
  regenerating this file by hand after every device-list change. As of
  this shim's `build_fake_interrupts()`, that's automatic: on first
  `/proc/interrupts` open per process, it reads the *real* file, filters
  to `MSI`/`Edge` lines (matches both plain `MSI` and `ITS-MSI` — the
  controller label varies by kernel build; confirmed directly switching
  from the Ubuntu cloud kernel to Debian trixie's — see
  [scripts/build_scripts/get_debian_trixie_kernel.sh](../../../../scripts/build_scripts/get_debian_trixie_kernel.sh)),
  verifies each candidate with a real no-op read-then-write-back probe
  (not just `access(W_OK)` — that's what the `arm-pmu`-style `EIO` case
  above slips past), then relabels six of them with the fake names,
  served from an anonymous `memfd` so nothing touches the real
  filesystem. Picked once and cached per process, so repeated lookups
  within one boot stay consistent.

  That probe alone isn't sufficient either, confirmed directly: a
  candidate can pass the probe at `/proc/interrupts`-read time and still
  fail Engine's actual affinity write moments later with the same
  `EPERM`, because Engine doesn't set affinity itself — it shells out
  (`sh -c "echo ... > .../smp_affinity_list"`) — and the real MSI vector
  our fake name landed on can transition from freely-reaffinitizable to
  pinned once whatever real virtio device actually owns it finishes its
  own init, on its own schedule, independent of when Engine gets to it.
  The shim now also intercepts `write()` and fakes success for
  `smp_affinity`/`smp_affinity_list` writes targeting the specific real
  IRQ numbers it mapped, propagated to the shelled-out child via an
  environment variable (`DTSHIM_FAKE_IRQS`) the same way `LD_PRELOAD`
  itself reaches that child — the child is a fresh `exec` of `/bin/sh`,
  not a fork continuing Engine's own memory, so it never reads
  `/proc/interrupts` itself and needs the mapping handed to it.

  This file on disk is now only consulted if the dynamic path fails
  outright (e.g. the real `/proc/interrupts` can't be read at all, or
  zero `MSI`/`Edge` candidates exist) — at that point it's exactly as
  stale/fragile as before, and the manual regeneration recipe still
  applies: boot the guest, `cat /proc/interrupts` (with the shim *not*
  loaded, or read the file directly) to see the real current numbering,
  then confirm which candidates are actually writable before reusing
  them:
  ```sh
  for i in <candidate IRQ numbers>; do
    printf 'IRQ %s: ' "$i"
    echo 1 > /proc/irq/$i/smp_affinity 2>&1 && echo OK || echo FAIL
  done
  ```
  Only reuse `OK` IRQs in the fake file, one real IRQ per fake name (six
  needed total).

Deploy via an `engine.service` drop-in rather than editing the vendor
script in place:

```sh
mkdir -p /etc/systemd/system/engine.service.d
cat > /etc/systemd/system/engine.service.d/override.conf << 'EOF'
[Service]
Environment=LD_PRELOAD=/root/dtshim_rmz2.so:/root/drmatomic.so
Environment=QT_QPA_PLATFORM=eglfs
EOF
systemctl daemon-reload
```

`drmatomic.so` (see `../drmatomic.c`) is required alongside this
shim for `eglfs` to actually produce visible output — without it Engine
starts fine but the screen stays black. See BUILDING.md's arm64/RK3588
section for why.