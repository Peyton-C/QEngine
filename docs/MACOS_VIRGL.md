# Build QEMU with Virgl on macOS
Homebrew's distrobution of QEMU doesn't include support for Virgl, required for proper acceleration of emulated QEngine enviroments.

A build script for UTM QEMU is in [`/scripts/build_scripts/build_virgl_qemu_macos.sh`](/scripts/build_scripts/build_virgl_qemu_macos.sh), to automatically build UTM QEMU 10.0.12 with it, run:
```
scripts/build_scripts/build_virgl_qemu_macos.sh
```