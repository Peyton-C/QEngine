# Test scripts
These scripts boot an instance headless, play test tones through Engine and check what came out. They are for audio first, but boot, sshd and tap work for any instance. They are not a test suite, as nothing replaces watching Engine run, but they make a run repeatable and its result a number.

Engine's audio is captured with QEMU's wav backend instead of played, so a run needs no speakers and works the same on the Mac and over ssh on a test host. A gap in the file is a gap Engine would have played. Captures, logs, screenshots, tone images and the guest key go to `build/test/`, or to `QENGINE_TEST_DIR` when it is set.

## Scripts
| Script | What it does |
| --- | --- |
| `make_tones.sh [MINUTES]` | Writes `usb.img`, a FAT32 stick with a 1000 Hz and a 440 Hz tone, 5 minutes each by default. macOS only. |
| `boot.sh NAME [LABEL]` | Boots an instance in the background with software rendering, serial on 4575, monitor on 4576, audio captured to `LABEL.wav` |
| `sshd.sh` | Starts sshd in the booted guest from its serial console, with the key under `/run` |
| `attach.sh PORT [IMAGE]` | Copies a tone image into the guest and attaches it as a loop device, which Engine sees as a USB stick, then prints the sound cards and alsashim settings |
| `setenv.sh PORT [VAR=value ...]` | Restarts Engine with extra environment from a drop-in under `/run`. With no variables it clears them. |
| `tap.sh HOST::PORT OUT.png [X Y ...]` | Taps each point over VNC, holding each tap, then saves a screenshot |
| `trial.sh [-H HOST] NAME SECONDS [VAR=value ...]` | Loads the first track on decks 1 and 2 by touch, plays both and checks the capture |
| `deploy.sh HOST NAME` | Copies an instance, its kernel, these scripts, the key and `usb.img` to a test host, then boots it and runs `sshd.sh` and `attach.sh` there |
| `wavcheck.py FILE [--tone] [--last SECONDS]` | Reports gaps, and with `--tone` the pitch per second and any discontinuities |
| `serial.py PORT 'command' [TIMEOUT]` | Runs one command on a guest's serial console and returns its output and exit code |
| `common.sh` | Paths, ports and the `guest`, `vnc` and `instance_var` helpers, sourced by the rest |

## A run on the Mac
```
scripts/test/make_tones.sh
scripts/test/boot.sh prime4-5.3.0
scripts/test/sshd.sh                       # once the console is up, about a minute
scripts/test/attach.sh <ssh port>
scripts/test/trial.sh prime4-5.3.0 60
```

Pass `QEMU_EXTRA_ARGS=-snapshot` to `boot.sh` to leave the instance's disks untouched. To stop the guest, send `quit` to the monitor on port 4576.

On the Mac, arm64 runs under HVF and armv7 under TCG, as Apple Silicon has no AArch32. To hear CoreAudio instead of checking a file, start the instance with `run_instance.sh` and leave `AUDIODEV_BACKEND` unset.

## A run on a test host
Build on the Mac and run on the host, which needs only QEMU, ssh and Python 3. `deploy.sh` keeps a copy of `scripts/` and the shared kernels under `~/qengine` on the host, or `QENGINE_REMOTE`, so it works on a fresh one:
```
scripts/test/deploy.sh user@host prime4-5.3.0
scripts/test/trial.sh -H user@host prime4-5.3.0 60
```

With `-H`, the taps go from the Mac over VNC and the capture is checked on the host. Only one guest runs on a host at a time, as every guest uses the same serial and monitor ports. Override them with `SERIAL_PORT` and `MONITOR_PORT` to run more.

## Things to know
- Engine ignores touch for about 90 to 110 s after it starts or restarts. `trial.sh` waits that out after `setenv.sh`, and nowhere else.
- `trial.sh` taps Modern Engine's 1280x800 layout and expects the library to show the stick's files. Dismiss first-run screens, update notices and "Incompatible Format" dialogs first. JP24 raises one of those for each of its own non-FAT disks and one for the stick's whole-disk device, so tap Ok until the library is clear.
- A wav capture at 44.1 kHz shows a 1000 Hz tone as 1000 Hz and a 44-sample period. A steady 999 or 1001 is zero-crossing rounding, while a 0.2% shift, such as 998, is a real rate error.
- Legacy Engine's VNC server crashes Engine when a client that took the rich cursor disconnects. Every script here calls vncdo with `--nocursor` for that reason. Legacy screenshots show red and blue swapped, which comes from vncdo, not Engine.
