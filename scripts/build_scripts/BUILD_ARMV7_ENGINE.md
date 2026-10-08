# Setup everything required to emulate armv7 Engine OS
Requires docker, qemu, binwalk 3.1.X, e2fsprogs.

This is Engine OS on the RK3288 controllers (Prime, SC, Mixstream), as opposed to
[BUILD_ARM64.md](BUILD_ARM64.md), which is the same application on RK3588. The two
need different rootfs builders and different disk layouts; `new_instance.sh` picks
between them from the firmware itself.

The short way is one command, which does all of the below and keeps each device in its
own directory — see [INSTANCES.md](INSTANCES.md):

```sh
scripts/build_scripts/new_instance.sh --name jp07-5.0.4 --firmware PATH_TO_PRIME_UPDATE.img
scripts/qemu/run_instance.sh --name jp07-5.0.4
```

The steps individually, if you want them:

1. [Download Engine from InMusic](https://enginedj.com/downloads) for an RK3288 controller.
2. Build the rootfs with `build_armv7_engine_rootfs.sh --firmware PATH_TO_PRIME_UPDATE.img`.
3. Get the kernel and initrd with `get_kernel.sh --arch armhf`.
4. Make a /data disk with `make_disk.sh --family mpc` — see the note below on why it
   is not `--family engine`.
5. Boot with `DEVICE=engine ARCH=armhf scripts/qemu/run_qemu.sh`.
   `DISPLAY_MODE` picks the display backend (`sdl`, `cocoa`, `vnc`, `none`, and the GL
   modes `sdl-gl` and `egl-vnc` — which work here now, see below). `VIRGL=off` forces
   the non-GL member of whichever pair is in play; `run_instance.sh` spells the same
   thing `--no-gl`.

## Notes

- **The /data disk uses the `mpc` layout, not the `engine` one.** This rootfs's
  `data.mount` asks for PARTUUID `931ad49d-ad59-0849-833a-9bf00af5b60e`, the single
  `az01-internal` partition, which is what the MPC images use — not the RK3588
  `data`+`factory` pair. Disk layout tracks the platform generation, not the
  application. `new_instance.sh` gets this right on its own.

- **One image, several products.** A single update image serves multiple device
  identities (`JP07-JP08-JP11-5.0.4.img` covers all three), and `/usr/Engine` is
  shared across them, so the devicetree product code is the only thing that
  distinguishes them. `PRODUCT_CODE=JP11 build_armv7_engine_rootfs.sh ...` picks
  one; the default is `JP07`. Only `JP07` has actually been booted.

- **Rendering goes through virgl to the host's GPU**, in a GL display mode
  (`egl-vnc` or `sdl-gl`). It was software for a long time, and the difference is
  not marginal — it is the difference between a slideshow and a usable UI. Two
  things had to land — the machine gained a working PCI bus with `highmem=off`, so
  `virtio-gpu-gl-pci` can be attached, and `build_virgl_mesa.sh` builds the DRI
  driver the guest needs at the guest's own Mesa version, because the vendor Mesa has
  no virgl in it and Debian's packaged driver cannot load here (see that script for
  why). That version and the driver layout come from `detect_mesa.sh`, which reads
  them off the rootfs rather than assuming: armv7 has shipped no Mesa at all
  (pre-5.0.0 — GL came from a proprietary Mali blob) and Mesa 24.0.7 in the DRI
  layout (5.0.x), while arm64 has shipped both layouts, so neither follows from the
  architecture. A non-GL mode such as
  `vnc` still rasterizes on the guest CPU.

- **The host needs a QEMU with virglrenderer.** Debian's arm64 build has neither
  `virtio-gpu-gl-pci` nor `egl-headless`, so the GL modes fail at startup there
  even though the change above is in place. Building QEMU from source with
  `--enable-virglrenderer --enable-opengl --enable-slirp` fixes it; `--enable-slirp`
  is not optional, since without it `-netdev user` disappears and no instance can
  boot.

- **Engine needs its EGL device integration named explicitly** —
  `QT_QPA_EGLFS_INTEGRATION=eglfs_kms`, plus `EGL_PLATFORM=gbm` and
  `MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu`. The rootfs build writes all three into
  `engine.service.d/override.conf`. Left to itself Qt picks no integration at all
  and Engine dies in an EGL restart loop; see
  [../../docs/BUILDING.md](../../docs/BUILDING.md#engine-504-on-armv7-rk3288).

- **The shims are shared with the arm64 build**, except `dtshim.c`, which
  carries RK3288's devicetree paths. `drmatomic` and `touchbridge` build from the
  RK3588 sources. One 32-bit-specific catch is worth knowing before writing another
  shim here: this guest's glibc is a 64-bit-`time_t` build, so it imports
  `__ioctl_time64` rather than `ioctl`, and an `LD_PRELOAD` interposer has to export
  both names or it loads and silently never runs.

- Used directly as above, the rootfs is written to `build/rootfs_out.img` — the same
  path the arm64 and MPC builds use, so building one target overwrites the other.
  Use an instance (see [INSTANCES.md](INSTANCES.md)) to keep several side by side.

- **Touch works out of the box**, same mechanism as the MPC build: the rootfs build
  installs `touchbridge` and starts it before `engine.service`. Engine only
  responds to a real touchscreen, and QEMU's virtio tablet presents as an absolute
  pointer, so the bridge re-emits it as a uinput multitouch device. Confirmed against
  the `sdl` display.

- **Audio is wired up, but has not been heard yet.** `alsashim` and `midisurface`
  are carried over from the arm64 build and are no longer RMZ2-specific -- they
  live in `shims/alsashim/` and `shims/midisurface/`, build for either
  architecture, and between them give Engine a control surface it will bind. Only
  `controllermap` is left out, which exists to swap a real USB controller's
  assignment files in and hardcodes RMZ2's directory.

  Three things stopped this short of audio. The machine had no working PCI to
  hang a sound card off; it does now, for the same `highmem=off` reason virgl
  does.

  `alsashim` was preloaded for one job only, the MIDI card-number gate, because
  the name Engine would accept for a sound card was unknown. It is the ASoC card
  name the vendor machine driver registers -- `JP07` for JP07, and also for JP08,
  which shares its `inmusic,jp07-audio` compatible -- so the rootfs build now
  detects it (`detect_audio_card.sh`) and sets `ALSASHIM_AS` alongside
  `ALSASHIM_CARD=0`. Engine's ALSA calls are identical to the arm64 build's,
  symbol for symbol, so the rest of the shim -- the `plughw` rewrite and the
  ring-depth scaling -- applies unchanged.

  **And the card itself had to change.** Debian builds `snd_hda_intel` for
  `linux-image-arm64` but not for `linux-image-armmp`: the armhf kernel ships
  `snd_hda_core`, `snd_hda_codec`, `snd_hda_codec_generic` and even
  `snd_hda_tegra`, but no Intel/PCI controller driver, so `ich9-intel-hda`
  enumerates on the bus and nothing ever binds it. No card is registered at all,
  which fails one step removed from its cause -- the shim's name spoof never
  runs, and what you see instead is Engine failing to resolve
  `/sys/class/sound/card0/device` plus alsa-lib's `Cannot get card index for 0`.
  armhf therefore gets `virtio-sound-pci,streams=1`, whose driver is built for
  both architectures; `streams=1` is how this device expresses the playback-only
  requirement that `hda-output` expresses on arm64. This needs a QEMU 8.2 or
  newer, and an initrd built after `virtio_snd` was added to `get_kernel.sh` --
  rerun `get_kernel.sh --arch armhf` if yours predates that.

- **Audio depth is an unresolved trade, and the numbers are not arm64's.** RMZ2's
  Engine asks for a 256-frame ring of 128-frame periods (5.8ms / 2.9ms); this one
  asks for 1024 frames of 512-frame periods — 23.2ms and 11.6ms — so each step of
  `ALSASHIM_BUFFER_SCALE` is worth four times as much wall clock. Measured on
  JP11, 512-frame periods throughout:

  | scale | ring | result |
  |------:|-----:|--------|
  | 1 | 23.2ms | `Audio_probe` frozen; Engine acts as though it has no audio device and will not load a track |
  | 4 | 92.9ms | XRUN before the first refill — `hw_ptr == appl_ptr == 4096` |
  | 8 | 185.8ms | plays, but the playhead visibly snaps backwards |
  | 32 | 743ms | same, much worse |

  The build ships **8**, the only value yet seen to produce sound.

  Read the scale-4 pointers carefully, because they say this is not Engine
  failing to keep up with a steady stream: `appl_ptr` stopped at exactly one ring
  and never moved again. Engine stalls *once* — at track load, from inside TCG,
  since a 32-bit guest gets no hardware acceleration on Apple Silicon — and never
  returns, because a PCM in XRUN state fails every later `snd_pcm_writei` with
  `-EPIPE` and Engine calls no `snd_pcm_prepare`. On real hardware that recovery
  path never has to work. So the ring depth is not buying throughput; it is
  buying enough slack never to hit that one fatal stall.

  Which is also why depth costs what it does. Engine never measures the latency
  it got — no `snd_pcm_delay`, no `snd_pcm_status` anywhere in the binary — so it
  advances the playhead by what it has written and then corrects to where the
  audio actually is. That correction is a backwards jump the size of the ring's
  fill level, which is precisely what deepening the ring enlarges.

  **`ALSASHIM_XRUN_FREE_RUN=1` is the way out of the trade, and is untested.** It
  sets playback's `stop_threshold` to the ring boundary, ALSA's idiom for "never
  stop on an underrun", so a stall becomes an audible gap instead of permanent
  silence — which should allow scale 1 or 2 and take the playhead error away with
  the depth. Try it against the table above; both are service environment
  variables, so a drop-in plus `systemctl restart engine` is the whole loop.

  `/proc/asound/card0/pcm0p/sub0/status` gives the state and XRUN count,
  `hw_params` beside it the ring actually granted. Past scale 8 the card also
  caps out — `virtio_snd` builds its ALSA constraints from module parameters at
  probe and `pcm_periods_max` defaults to 16, i.e. 8192 frames — so
  `arch_devices.sh` raises it on the kernel command line
  (`virtio_snd.pcm_periods_max=128`) to keep that clamp from being silent. It has
  to be the command line rather than `/etc/modprobe.d`, because `virtio_snd` is in
  the initrd's own load list and is modprobed inside the initramfs, before the
  real root's `/etc` exists.

  What has *not* happened is a boot with sound coming out. Run with
  `ALSASHIM_DEBUG=1` and `QT_LOGGING_RULES=air.devicemanager.*=true`: the shim
  should log `reporting card name "..." as "JP07"` (if it does not, there is
  still no card -- check `/proc/asound/cards` and `dmesg | grep virtio_snd`), and
  Engine should get past `Get card info for hw:0` to `Query device 0` /
  `Device name hw:0`.
