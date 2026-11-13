# MPC Sample
QEngine boots MPC Sample 1.3.0 as `AC50` on RK3566 arm64, with a virtual panel and host audio. Install Docker, QEMU, Python 3, e2fsprogs and `file`; [Build](../../docs/BUILDING.md) covers host setup.

## Build
Run from the repository root with Docker running:

```sh
scripts/build_scripts/new_instance.sh --name mpc-sample-1.3.0 \
    --product-code AC50 --firmware "/path/to/MPC Sample 1.3.0 Updater.app.zip"
scripts/qemu/run_instance.sh --name mpc-sample-1.3.0 --display vnc
```

QEngine detects the `mpc-sample` family and uses `build_mpc_rootfs.sh`. Each instance owns its rootfs and `/data`/`/factory` disk. `--force` rebuilds the rootfs and keeps that disk; see [Instances](INSTANCES.md).

| Item | Status |
| --- | --- |
| Application | `ac50.service` reaches the sample trim screen |
| Display | 320×240 landscape, software rendering; no SPI panel emulation |
| Input | Virtual panel; no touchscreen or mouse input |
| Audio | First stereo pair plays through the host; inputs are silent |
| Content | Bundled temporary project loads; separate factory content partition absent |
| TEE | No extra shim needed to start the interface |

Only arm64 `AC50` firmware 1.3.0 has been booted.

## Controls
Write commands to the panel FIFO in the guest:

```sh
echo 'pad 6 120' > /run/mpcsurface.fifo
echo 'set volume 127' > /run/mpcsurface.fifo
```

The [mpcsurface reference](../../shims/mpcsurface/README.md#mpc-sample) lists the buttons, knobs and startup-volume setting. The firmware updater can report no panel before `ac50.service` starts; that does not prevent the application from using the virtual panel.

## Audio
QEngine loads `snd-dummy` with the `qengine-ac50` model and exposes it as `/dev/snd/by-path/platform-sound`. It supplies the four-channel S32_LE, 44.1 kHz format the Sample opens. `alsashim` copies the first pair, speaker and line output, to QEMU's HDA card.

If the kernel lacks that dummy-card model, stop the guest and run `scripts/build_scripts/get_kernel.sh --arch arm64`, then boot again. The launchers configure CoreAudio at 48 kHz to avoid clicking on host devices that run at that rate. Playback was heard on macOS; capture remains silent.
