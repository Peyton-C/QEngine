# Working in this repo

## Naming
The project is always written `QEngine`, one word, capital Q and E, in prose, headings, code comments and commit messages. The repo directory and anything conventionally lowercase use `qengine` (the Pi's `~/qengine`, `qengine.service`), and those are the only exceptions.

`Engine` is inMusic's DJ application and `Engine OS` the distro it ships on; keep the two apart. The company is `inMusic`. Devices are written as their makers write them: `RANE SYSTEM ONE`, `Prime 4 G2`, `MPC Live Mk 2`. Product codes are written as the devicetree holds them, `JP24`, `RMZ2`, `ACV5`. The mod loader is `MONTERO`, in capitals.

## What goes where
`README.md` is what someone needs to build an instance, boot it and see what runs, nothing more. It keeps the quick setup commands and the device tables, because what boots on which version is the first thing a reader looks for. It does not carry the depth.

`docs/` holds one file per subject, and those files own it: building, Engine OS internals, telemetry, remote access, virgl on macOS. Instructions tied to one place live beside it instead: each target's `BUILD_*.md` and `INSTANCES.md` in `scripts/build_scripts/`, the Pi in `scripts/pi/README.md`, the FLX10 in `scripts/pi/flx10/README.md`. `shims/README.md` gives each shim one line, and a shim that needs more gets its own README, as `dtshim` does. A doc should stand on its own rather than assume the reader came through another one.

Technical detail is welcome in `docs/`, more so than in the README. Which devicetree properties Engine reads, how a rootfs is assembled and what each shim interposes belong there.

That is mechanism, not evidence, and the distinction matters. That arm64 Engine 5.1.0 and later quits to TestApp without `teeshim` is mechanism and can be documented. The disassembly that found the TEE check is evidence, and stays out under the rule below.

`build/` is the working directory and is not tracked: firmware, instances, kernels and research notes. Anything an agent or another person needs to know goes in `docs/` or this file.

## Documentation voice
The house style is pink diamond's, the same as bejeweled's. Most of the docs here were written before this file and do not follow it yet; `scripts/pi/README.md` is closest. Bring a doc into line when you rewrite it, rather than restyling one in passing.

**Be short.** Roughly half the length of a first draft. State what QEngine does and, if it is not obvious, why. Then stop.

**Cut the evidence, keep the consequence.** The test is whether it changes what the reader does. Measurements, worked examples and the reasoning that justifies a decision come out. A caveat that would change someone's choice stays in, even when it is unflattering: SoundSwitch does not work on the Pi, and someone planning to play out from one needs to know that.

**Name the tool as the subject.** "QEngine builds...", "`dtshim` serves...", "Engine reads...". Prefer that to passive voice or an abstract subject.

**Address the reader directly** for anything they do: "point `--firmware` at the update image", "copy `pi.env.example`". A reference section is mostly descriptive and will barely use it, which is fine, do not manufacture instructions to satisfy the rule.

**Use a table for any fixed set a reader might look up**: devices and the versions that boot, product codes, shims, `pi.env` settings, the source layout. Prose gets trimmed; tables do not.

### Mechanics
- No hard wrapping. One long line per paragraph, however wide it runs.
- No blank line between a heading and the text under it.
- Commas where a dash would be tempting. No em-dashes.
- No bold or italics in prose. Emphasis comes from sentence structure.
- Headings are short noun phrases: "Build", "Instances", "Stopping".

### Common corrections
Drafts written without this file in mind tend to need the same fixes: they run about twice the necessary length, wrap lines, lean on em-dashes for asides, and explain the reasoning behind a decision where stating the decision would do.

Once a doc has been rewritten to this style, it is right where it contradicts this file, and this file should be updated.

## Code
QEngine runs inMusic's firmware unmodified, in QEMU or in a chroot on a generic arm linux system:

```
update .img  ->  new_instance.sh  ->  build/instances/<name>/  ->  run_instance.sh    QEMU
                 extract, pick a      rootfs, disks,                stage_for_pi.sh   Pi chroot
                 builder, shims,      instance.env
                 rootfs_steps
```

| Path | What |
| --- | --- |
| `scripts/build_scripts/` | `new_instance.sh`, a rootfs builder per family and architecture, the kernel builder, and patches to QEMU, Mesa and qtwayland |
| `scripts/build_scripts/rootfs_steps/` | Steps every builder sources into its privileged container: telemetry, devicetree, MONTERO, resizing |
| `scripts/qemu/` | `run_instance.sh` reads an instance and execs `run_qemu.sh`, the only QEMU command line |
| `scripts/pi/` | Staging an instance for the Pi and running it there, its services and sway configs |
| `scripts/vm/` | Run inside a guest, as root, to start Engine by hand |
| `shims/` | `LD_PRELOAD` shims and helpers in C, with SoC-specific ones under `rk3288/` and `rk3588/` |

Engine itself is never patched. Everything QEngine changes is done around it: shims preloaded into it, files written into the rootfs at build time, and a devicetree served by `dtshim`. MONTERO follows the same rule, a mod carries only its own QML and never a copy of Engine's. Firmware, rootfs images and anything extracted from them are inMusic's and are never committed.

Telemetry stays blocked. Every rootfs a builder writes goes through `block_telemetry`, so nothing a guest does reaches inMusic's Sentry or analytics. A new host goes in the list in `rootfs_steps/block_telemetry.sh`, which is the only list.

An instance owns everything that differs between devices, so two can be built and booted side by side. Kernels are the one thing shared, one per architecture. A build option that should survive `--force` is recorded in `instance.env`, as `--product-code` and `--montero` are. Device families are rows in the registry in `new_instance.sh`, so a new family or architecture is a new row, not a new script.

A rootfs step is sourced, never run on its own, and must be safe to run twice over the same image. Shims are built by the builders for the target architecture inside Docker. A shim that differs by SoC keeps one source and picks with `-DSOC_RK3588` or `-DSOC_RK3288`, as `dtshim` does, rather than being copied. A new shim gets its line in `shims/README.md`.

On the Pi, settings come from `pi.env`. A new one is documented in `chroot-engine.sh` and added to `pi.env.example`, commented out. Nothing written for a session, such as an sshd config or keys, goes into the rootfs; it lives under `/run` or `/srv/engine/run`.

There is no test suite. Run `shellcheck` over changed scripts and `bash -n` at least. Python, such as the FLX10 bridge, runs with `uv run`. The Pi used for chroot runs is `pi-engine.local`, with the staged files in `~/qengine`. Nothing replaces booting Engine, so say which architecture, product code and firmware version a change was tried on, and which it was not.

Code comments are the place for the reasoning the docs leave out. Explain why something is done a particular way, especially where the obvious approach fails, and cite the firmware version or device that shows it. The long header comments on the build scripts are the model.

## Commits
The user commits their own work. Leave changes in the working tree and suggest the `git add` and `git commit` commands.

Commit messages, when suggested, follow the log: a subject line in the imperative, then, only when it is not obvious from the diff, a short clause after ` - ` on why or what it was tested against. "Build the MONTERO modloader into VM images with --montero - Tested against armv7 and arm64 on 5.3.0 w/ JP24 and JP07". No essays, and no restating the diff as a list of touched files.
