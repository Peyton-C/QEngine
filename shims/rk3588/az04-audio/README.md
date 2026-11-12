# AZ04 audio
These experimental ASoC modules create an RMZ2-named ALSA card under QEMU. `az04_codec.c` supplies codec capabilities without GPIO or devicetree dependencies; `az04_card.c` pairs it with a software CPU DAI and platform. They do not emulate the Rockchip I2S controller or provide audible output. Normal Engine playback uses [alsashim](../../../docs/engine/AUDIO_MIDI.md).

## Build
Run from the repository root:

```sh
scripts/build_scripts/build_az04_audio.sh
```

The script uses Debian trixie kernel headers and builds the codec and card modules beside their source. It extracts the ASoC dependencies into `build/az04-audio/`.

| Module | Output directory |
| --- | --- |
| `az04_codec.ko`, `az04_card.ko` | `shims/rk3588/az04-audio/` |
| `snd-pcm-dmaengine.ko`, `snd-compress.ko`, `snd-soc-core.ko` | `build/az04-audio/` |

Build against the exact kernel package used by the guest. `get_kernel.sh --arch arm64` and this script fetch packages independently, so a later package update can leave them mismatched. Matching release names alone is insufficient when symbol versions differ. Use the same package versions for both builds.

## Guest
Transfer the modules through the running guest's network connection, following [Remote access](../../../docs/REMOTE_ACCESS.md). Do not write its disk image with `debugfs` while QEMU is using it. Remount `/` writable if copying into `/root`.

Place the five modules in one directory, then load them as root:

```sh
insmod snd-pcm-dmaengine.ko
insmod snd-compress.ko
insmod snd-soc-core.ko
insmod az04_codec.ko
insmod az04_card.ko
```

The guest needs no vendor `snd-soc-inmusic-az04.ko`. That driver depends on the real board's audio path, which QEMU's `virt` machine lacks. Userspace diagnostics must match the guest architecture and glibc; kernel-module compatibility is a separate constraint. See [Toolchain](../../../docs/BUILDING.md#toolchain).
