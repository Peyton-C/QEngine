# Setup everything required to emulate arm64 Engine OS

`PRODUCT_CODE=` selects which device identity the rootfs spoofs, the same way it does
in the armv7 builder; the default is `RMZ2`. `JP22` and `JP24` ride the same rootfs
and neither has been booted.
Requires docker, qemu, binwalk 3.1.X, e2fsprogs.

The short way is one command, which does all of the below and keeps each device in its
own directory — see [INSTANCES.md](INSTANCES.md):

```sh
scripts/build_scripts/new_instance.sh --name rmz2-4.6.0 --firmware PATH_TO_SYSTEMONE_UPDATE.img
scripts/qemu/run_instance.sh --name rmz2-4.6.0
```

The steps individually, if you want them:

1. [Download Engine from InMusic](https://enginedj.com/downloads) for the RANE SYSTEM ONE.
2. Build the rootfs with `build_arm64_rootfs.sh --firmware PATH_TO_SYSTEMONE_UPDATE.img`.
3. Get the kernel and initrd with `get_kernel.sh --arch arm64`.
4. Make a data disk for Engine with `make_disk.sh --family engine`.
5. Boot with `scripts/qemu/run_qemu.sh`, whose defaults match what those steps wrote.
   `DISPLAY_MODE` picks the display backend (`sdl`, `cocoa`, `vnc`, `sdl-gl`, `egl-vnc`),
   defaulting to the GL one where the platform has it. `VIRGL=off` forces the non-GL
   member of the pair and `VIRGL=on` the GL one; `run_instance.sh` spells those
   `--no-gl` and `--gl`. A GL mode this QEMU cannot serve is demoted with a warning
   rather than refused.
