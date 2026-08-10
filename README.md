# QEngine - Engine OS Emulator
Emulate Engine OS inside QEMU

**NOTE:** Docs are currently **awful**, they will be better eventually. Basic docs for setting up arn64 emulation are [here.](scripts/build_scripts/BUILD_ARM64.md)

## Documentation
- [BUILDING.md](docs/BUILDING.md) — building/assembling and booting an Engine OS image under QEMU
- [ENGINEOS.md](docs/ENGINEOS.md) — Engine OS internals, product spoofing, known limitations
- [BLOCKING_TELEMETRY.md](docs/BLOCKING_TELEMETRY.md) — stopping Engine's crash/analytics reporting from reaching InMusic's real Sentry project

## Engine OS Support Matrix
|                         | 5.0.4 | 4.6.0 | 4.3.0 |
|-------------------------|-------|-------|-------|
| Rootfs Extraction       | Y     | Y     | Y     |
| Engine                  | Y     | Y     | Y     |
| SoundSwitch             | Y     | Y     | N     |
| Native Display          | Y     | Y     | N     |
| QT VNC Display          | N     | N     | Y     |
| Fake Touch              | Y     | Y     | Y     |
| Keyboard Navigation     | Y     | Y     | Y     |
| Audio Playback          | N     | N     | N     |
| MIDI                    | ~     | ?     | ~     |
| External Media (USB/SD) | Y     | Y     | N     |

## Emulated Controllers
|                         | SOC    | Signed FW | Arch  | 5.0.4 | 4.6.0 | 4.3.0 |
|-------------------------|--------|-----------|-------|-------|-------|-------|
| Denon DJ Prime 2        | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Denon DJ Prime 4        | RK3288 | N         | armv7 | ?     | N/A   | Y     |
| Denon DJ Prime 4+       | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| Denon DJ Prime GO       | RK3288 | N         | armv7 | ?     | N/A   | !     |
| Denon DJ Prime GO+      | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC5000 Prime   | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC5000M Prime  | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC6000 Prime   | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC6000M Prime  | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC Live 2      | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| Denon DJ SC Live 4      | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| Numark Mixstream Pro    | RK3288 | N         | armv7 | ?     | N/A   | ?     |
| Numark Mixstream Pro+   | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| Numark Mixstream Pro GO | RK3288 | Y         | armv7 | ?     | N/A   | ?     |
| RANE SYSTEM ONE         | RK3588 | Y         | arm64 | Y     | Y     | N/A   |

| Denon DJ Prime 4 | RANE SYSTEM ONE |
|------------------|-----------------|
| ![Emulated Prime 4, Engine OS 4.3.0 About Screen](images/DENON_DJ_PRIME_4_ABOUT_430.jpg) | ![Emulated SYSTEM ONE, Engine OS 4.6.0 About Screen](images/RANE_SYSTEM_ONE_ABOUT_460.png) |
| ![Emulated Prime 4, Engine OS 4.3.0 Playlist Screen](images/DENON_DJ_PRIME_4_PLAYLISTS_430.png) | ![Emulated SYSTEM ONE, Engine OS 4.6.0 Playlist Screen](images/RANE_SYSTEM_ONE_PLAYLISTS_460.png) |
