# Fake Engine
The fake Engine is a small program that makes the calls Engine makes at QEngine's shims and reports, for each, whether it got what Engine needs. `check.sh` runs it inside an instance's own rootfs, with the guest's loader and libraries and the environment of the unit the builder put `LD_PRELOAD` on. A run takes a few seconds per instance, so a shim change can be tried against every firmware version before any of them is booted.

It replaces the application and nothing else. It does not replace booting Engine: it knows the call shapes the shims were written against and nothing newer, and it never touches Qt or the display.

## A run
Build instances as usual and copy them to a test host with `deploy.sh`, or `rsync` just their `rootfs.img` and `instance.env`. Then, from the Mac:
```
scripts/test/fake/check.sh -H peyton@qengine-1.local -s -b    # on a tree you trust, save the baseline
scripts/test/fake/check.sh -H peyton@qengine-1.local -s       # after editing a shim
scripts/test/fake/sweep.sh -s peyton@qengine-1.local peyton@qengine-2.local
```

`check.sh` prints one row per instance, then each failure and each result that differs from the baseline, with the instances that share it. It exits 1 on either. Each instance's full results are in `build/test/fake/results/HOST/NAME.tap`, and what the loader and shims printed is in `NAME.log` beside it.

| Option | Effect |
| --- | --- |
| `-H HOST` | Runs on that ssh destination. The scripts, the fake and any shims are copied there first. |
| `-s` | Uses shims built from the working tree in place of the instance's own. Without it, the shims are the ones the instance was built with. |
| `-g` | Runs in the booted guest, in place of its application, which is stopped for the run and started after |
| `-b` | Saves the results as the baseline |
| `-p a,b` | Runs only those probes |
| `NAME ...` | Instances to check. With none, every instance on the machine. |

`sweep.sh` takes the same options before a list of hosts and checks every instance on each.

## Chroot or guest
By default `check.sh` mounts `rootfs.img` read-only under a tmpfs overlay and chroots into it. That needs Linux and sudo, so on the Mac pass `-H`. The image is not written to, and an instance that is running is skipped. The kernel is the host's, so `/proc/interrupts` is the host's and the sound card is `snd-dummy`, which `check.sh` loads for the run because a real card does not grant Engine's 128-frame period.

With `-g` it runs in a guest brought up with `boot.sh` and `sshd.sh`, where the kernel, QEMU's sound card and the surface service are the ones Engine gets. Use it for audio changes. The tone the fake plays lands in the instance's wav capture.

Armv7 rootfs images run in a chroot on a 64-bit Pi 4 or Pi 5, and armv7 guests need a Pi 4 for KVM.

## Working-tree shims
`-s` runs `build_shims.sh`, which compiles the shims as the rootfs builders do and puts each one the unit preloads under `/run` in the guest, with `LD_PRELOAD` pointing there. Firmware older than glibc 2.34 gets shims linked against its own libc, which `check.sh` copies out of the instance first. `qmlrotate`, MONTERO, `maliwindow` and `qt55vnc` stay as installed.

`build_shims.sh` holds copies of the builders' compiler lines. Change a builder's and change it there.

## Probes
| Probe | What Engine does | Shim |
| --- | --- | --- |
| `loader` | Starts with every `LD_PRELOAD` entry loaded and its imports resolved | all |
| `identity` | Reads the product code and serial number, by open and by `access()` | `dtshim` |
| `devmem` | Opens `/dev/mem`, maps it and reads zeros | `dtshim` |
| `devicetree` | Reads panel rotation and the board properties for its SoC | `dtshim` |
| `drumcontent` | Finds the Core Drum Module's content flag as `LDMF`, and no other RK3288 product does | `dtshim` |
| `interrupts` | Finds six IRQs by name, then sets their affinity from a shell | `dtshim` |
| `efuse` | Stats, canonicalises and reads the RK3288 eFuse files | `dtshim` |
| `battery` | Reads a full battery on AC, on `JP11`, `JP11S` and `NH10` | `dtshim` |
| `tee` | Attests through OP-TEE, where the application links `libteec` | `teeshim` |
| `alsa` | Accepts the card by name, opens `hw:N`, sets a 256-frame ring of 128-frame periods at 44100 Hz and plays a second of tone | `alsashim` |
| `midi` | Finds the surface, requires a card number, sends a Device Inquiry and takes a note | `alsashim`, `midisurface` |
| `quit` | Gets `QCoreApplication::quit` on SIGTERM, and a splash client | `quitshim` |

`loader`, `identity` and `devmem` run for every application, `devicetree` also for the RK3566 ones, MPC Sample and HeadRush VX5, and `drumcontent` for every RK3288 one. The rest run for Engine. `surface` prints, as comments, which of each shim's exports the real application imports.

`drmatomic`, `cursorshim`, `qmlrotate`, MONTERO and the touch bridges are checked by `loader` only.

## Reading a result
One read reaches libc through a different symbol on each firmware: `open` or `open64`, `stat64` or `__stat64_time64` or `__xstat64`. The fake tries every entry point the guest's libc has and prints a line for each, then one for the read as a whole, which passes when an entry point the real application imports returns the right thing. Only that line can fail.

A single entry point that does not work shows as skipped, because Engine imports many it never points at a devicetree, and the fake cannot tell which it uses for what. So a broken or deleted interposer may not fail by itself. It does show as a change from the baseline, `was ok, now skip`, which is why the baseline is worth saving before a change.

## Without firmware
`standin.sh` runs the fake in stock distribution containers, one per firmware era, picked for their glibc. It needs Docker and nothing of inMusic's, so it is what the `Shims` GitHub workflow runs on every change under `shims/`.

| Stand-in | Image | Stands in for |
| --- | --- | --- |
| `modern-arm64` | Ubuntu 24.04, glibc 2.39 | arm64 Engine 4.4.0 onward |
| `modern-armv7` | Ubuntu 24.04, glibc 2.39, 64-bit `time_t` | armv7 Engine 5.0.0 onward |
| `classic-armv7` | Debian bookworm, glibc 2.36 | armv7 Engine 2.4.0 to 4.3.4, with a battery as `JP11` |
| `legacy-armv7` | Debian stretch, glibc 2.24 | Legacy Engine, with shims linked against the container's libc |
| `mpc-armv7` | Debian bookworm | MPC 3.x |
| `mpc2-armv7` | Debian buster, glibc 2.28 | MPC 2.x, with shims linked against the container's libc |
| `mpc-arm64` | Ubuntu 24.04 | Gen 2 MPC |
| `sample-arm64` | Ubuntu 24.04 | MPC Sample, with the RK3566 `dtshim` |
| `vx5-arm64` | Ubuntu 24.04 | HeadRush VX5, with the RK3566 `dtshim` |

The Engine stand-ins run `loader`, `identity`, `devmem`, `devicetree`, `efuse`, `battery`, `tee` and `quit`, and the others what their application gets from `check.sh`. The glibc of the three arm64 non-Engine firmwares was not looked up, so 24.04 is a guess for them. There is no sound card, sequencer or real application in a container, so `alsa`, `midi` and `interrupts` do not run, and every libc entry point counts as one the application might use. A distribution's glibc is close to the firmware's, not the same, so a pass here does not replace `check.sh` against instances.

Its baseline is tracked, in `standin/`, and holds statuses only. After a change that is meant to alter a result, run `standin.sh -b` and commit the difference.

## Other applications
A probe is a row in `PROBES` in `main.c` and a function in a `probe_*.c` file. Each row names the applications it is for, picked by the instance's `DEVICE`, so the shared ones run for MPC, MPC Sample, HeadRush and the drum module too. An MPC probe, for `mpcsurface` or the cue output, is one more row.

## Files
| File | What |
| --- | --- |
| `check.sh` | Runs the fake in instances and reports |
| `sweep.sh` | Runs `check.sh` on several hosts at once |
| `standin.sh` | Runs the fake in stock containers, with no firmware, and `standin/` holds its baseline |
| `build.sh` | Builds the fake for arm64 and armhf, on Debian jessie so glibc 2.22 loads it |
| `build_shims.sh` | Builds the working tree's shims for `-s` |
| `report.py` | Summarises results and compares them with the baseline |
| `unitenv.py` | Turns a unit's `Environment=` lines into exports |
| `main.c`, `variants.c`, `elf.c` | The program, its libc entry-point table and its ELF reader |
| `probe_*.c` | The probes |
