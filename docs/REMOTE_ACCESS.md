# Remote Development Workflow for Agents

A suggested workflow for reaching and driving a QEMU VM running on a remote
build host, separate from [BUILDING.md](/docs/BUILDING.md), which covers
building/booting the image itself.

A suggested workflow for reaching and driving a QEMU VM with an agent. 

## Driving the VM's console via tmux
Run QEMU as a tmux pane's foreground process (not inside a login shell), with
`-serial stdio` wiring the guest's serial console directly to that pane:

```sh
ssh <remote-host> 'tmux send-keys -t <session> "some command" Enter'
ssh <remote-host> 'tmux capture-pane -t <session> -p -S -30'   # last 30 lines of scrollback
```

Wait for a boot milestone with a poll loop rather than a fixed sleep:
```sh
until ssh <remote-host> 'tmux capture-pane -t <session> -p' | grep -q "login"; do sleep 3; done
```

**Gotchas:**
- **Never send Ctrl-C into this pane.** `-serial stdio` maps the host's
  stdin/stdout directly onto QEMU's own process, so Ctrl-C delivers SIGINT to
  QEMU itself — killing the whole VM, not whatever's hung in the guest. To
  stop the VM deliberately, kill QEMU from a separate SSH command instead:
  ```sh
  ssh <remote-host> 'pgrep -fa qemu-system-arm'
  ssh <remote-host> 'kill -TERM <pid>'
  ```
- **If QEMU exits or is killed, the tmux pane goes fully dead** (its
  foreground process is gone) — recreate the session and relaunch:
  ```sh
  ssh <remote-host> 'tmux kill-session -t <session> 2>/dev/null; tmux new-session -d -s <session> -c <repo_path>'
  ssh <remote-host> 'tmux send-keys -t <session> "<qemu-system-arm ... command>" Enter'
  ```
- Back-to-back `send-keys` calls with too-short sleeps between them can
  interleave command echoes in the scrollback — leave ~1-1.5s between sends
  when chaining several commands.

## Getting files into the guest
Run a plain HTTP file server on the remote host:
```sh
python3 -m http.server 8080
```
`scp` a file to the remote host, then from inside the guest:
```sh
wget -q http://10.0.2.2:8080/<filename> -O /root/<filename>
```
`10.0.2.2` is QEMU user-mode networking's default gateway, routing to the
host — this works regardless of any `hostfwd=` port mappings.

The guest's root filesystem mounts read-only by default; remount read-write
before writing anything:
```sh
mount -o remount,rw /
```

## USB passthrough device permissions
Real USB hardware passed through via QEMU's `usb-host` device needs its host-side `/dev/bus/usb/BBB/DDD` node readable/writable by the user running QEMU. Scope a udev rule to only the vendor IDs actually in use:

```
# /etc/udev/rules.d/99-qengine-usb.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="154e", MODE="0666"   # Denon/Marantz
SUBSYSTEM=="usb", ATTR{idVendor}=="2b73", MODE="0666"   # AlphaTheta/Pioneer DJ
```

Add a line per vendor ID (`lsusb` to find it), then:
```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```
