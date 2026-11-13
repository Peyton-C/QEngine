# Remote access
Run QEngine on a build host and reach its serial console through SSH and tmux. Use the instance launcher so disks, ports and display settings come from the instance. [Build](BUILDING.md) covers preparation; [Instances](../scripts/build_scripts/INSTANCES.md) lists launch settings.

## Console
From your workstation, connect to the build host and open a persistent tmux session:

```sh
ssh -t user@build-host 'tmux new-session -A -s qengine'
```

In that session, change to the repository and launch the guest:

```sh
cd /path/to/QEngine
scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display egl-vnc
```

The terminal becomes the guest's serial console. Log in as `root` without a password. Detach with tmux's `Ctrl+B`, then `D`; reconnect with the same SSH command. The session's shell remains available after QEMU exits.

Use `--display vnc` if the host lacks a virgl-capable QEMU. Software rendering is slower. Legacy Engine prints a separate `Engine VNC` address for its interface; QEMU VNC shows the console.

## VNC and SSH
The launcher prints the ports. From your workstation, forward the applicable VNC port through SSH, replacing both occurrences of `5991` with the printed TCP port:

```sh
ssh -N -L 5991:127.0.0.1:5991 user@build-host
```

Connect your VNC viewer to `127.0.0.1:5991`. For legacy firmware, forward the printed Engine VNC port instead. Keep the tunnel open while using the viewer.

If the guest runs an SSH server, the launcher's printed `ssh -p PORT root@localhost` command works on the build host. From your workstation, forward that port through the build host in the same way. A port forward does not install or start a guest SSH server.

## Files
Create a transfer directory on the build host and copy your file there:

```sh
ssh user@build-host 'mkdir -p /tmp/qengine-transfer'
scp diagnostic.bin user@build-host:/tmp/qengine-transfer/
```

Start the server from your workstation, leaving this SSH connection open:

```sh
ssh user@build-host 'python3 -m http.server 8080 --directory /tmp/qengine-transfer'
```

In the guest, QEMU user-mode networking exposes the build host at `10.0.2.2`:

```sh
wget -q http://10.0.2.2:8080/diagnostic.bin -O /data/diagnostic.bin
```

For a destination on the read-only rootfs, run `mount -o remount,rw /` first. Use the guest filesystem for live transfers; do not write an image with `debugfs` while QEMU has it open. Stop the HTTP server when finished. If guest SSH is available, you can use `scp` through its forwarded port instead.

## USB
On a Linux build host, QEMU needs read/write access to the USB device node. Find its vendor and product IDs with `lsusb`, then add a rule under `/etc/udev/rules.d/`. Replace `VVVV`, `PPPP` and `YOUR_GROUP` with the device IDs and an existing group your build-host account belongs to:

```text
SUBSYSTEM=="usb", ATTR{idVendor}=="VVVV", ATTR{idProduct}=="PPPP", GROUP="YOUR_GROUP", MODE="0660"
```

Reload the rules and reconnect the device:

```sh
sudo udevadm control --reload-rules
```

Pass it to an instance with the same IDs:

```sh
QEMU_EXTRA_ARGS='-device usb-host,vendorid=0xVVVV,productid=0xPPPP' \
    scripts/qemu/run_instance.sh --name rmz2-5.0.4 --display egl-vnc
```

Permissions and passthrough make the device available to the guest; the application still needs its own supported protocol or controller mapping. See [Audio and MIDI](engine/AUDIO_MIDI.md) for Engine controllers.

## Stopping
Run `poweroff` in the guest and wait for QEMU to exit. Stopping QEMU from the host skips normal guest shutdown, so do not use `Ctrl+C` as the routine stop command. Before rebuilding or copying disks, check the build host with `pgrep -fl qemu-system` and stop any temporary transfer servers or tunnels you started.
