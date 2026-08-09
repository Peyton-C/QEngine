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
- `interrupts` — fake `/proc/interrupts`, remapped in place of the real one.
  Engine hard-throws (`std::runtime_error`, aborts) if it can't find IRQ
  lines for `dwc3`, `fe210000.sata`, `fea10000.dma-controller`,
  `ff0c0000.dwmmc`, `ff0f0000.dwmmc`, and `ttyS0` by name — none of which
  exist under QEMU's `virt` machine. This file fakes all six, each reusing
  a real IRQ number already present in the guest so the CPU-affinity write
  Engine does immediately after finding each one lands on a real
  `/proc/irq/<N>/` directory instead of failing a second time.

  **IRQ numbers are not stable across QEMU device-list changes** (they
  shifted once already, from a 4-CPU config to the current 8-CPU one) —
  not every real IRQ accepts an `smp_affinity` write either (GPIO-backed
  and per-CPU IRQs like `arm-pmu` return `EIO`). If Engine starts
  crash-looping again on "Failed to set CPU affinity for IRQ ... ", the
  mapping has drifted and needs regenerating: boot the guest, `cat
  /proc/interrupts` (with the shim *not* loaded, or read the file directly)
  to see the real current numbering, then confirm which candidates are
  actually writable before reusing them:
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
Environment=LD_PRELOAD=/root/dtshim_rmz2.so:/root/drmatomic_rmz2.so
Environment=QT_QPA_PLATFORM=eglfs
EOF
systemctl daemon-reload
```

`drmatomic_rmz2.so` (see `../drmatomic_rmz2.c`) is required alongside this
shim for `eglfs` to actually produce visible output — without it Engine
starts fine but the screen stays black. See BUILDING.md's arm64/RK3588
section for why.