#!/usr/bin/env python3
"""powerkey.py — lets Engine's "Turn Off" prompt decide when this machine powers off.

Usage: systemd-inhibit --what=handle-power-key --mode=block powerkey.py <engine-root>
  Started by chroot-engine.sh, on the host and not in the chroot, once Engine is
  launched. Runs for as long as that Engine does.

Engine reads the machine's power button like any other key, and answers it the
way it does on its own hardware: a "Turn Off" prompt, and a clean exit if the
answer is Yes. Two things keep that from working on an ordinary machine:

  - the login manager (systemd-logind) acts on the same press and powers off at
    once, prompt or no prompt. The inhibitor lock in the usage line is what
    stops that, and it lasts exactly as long as this process -- with Engine
    gone, the button is the machine's own again.
  - after a Yes, Engine's launch script runs `systemctl poweroff`, which does
    nothing from inside a chroot.

So this does the powering off, in the two cases where it is wanted:

  - Engine exits having given "Poweroff" (or "Reboot") as its reason. It writes
    the reason to /tmp/engine-quit-reason, where its launch script reads it.
  - the button is pressed again while the prompt is up, as a desktop's shutdown
    dialog would have it. Engine creates the reason file when it shows the
    prompt, rewrites it in place on further presses, and removes it on No: a
    press that finds the same file as the press before it found the prompt
    still up.

A button Engine is not answering at all (no reason file appears) powers off on
two presses within a few seconds, so a hung Engine cannot take the button away.
"""
import os
import select
import struct
import subprocess
import sys
import time

KEY_POWER = 116
EV_KEY = 1
EVENT = struct.Struct("qqHHi")  # struct input_event on a 64-bit kernel
SETTLE = 0.3    # seconds for Engine to act on a press before looking
UNANSWERED_WINDOW = 5.0


def log(message):
    print(message, flush=True)


def find_engine(timeout):
    """Pid of the Engine process, waiting up to timeout seconds for it."""
    deadline = time.monotonic() + timeout
    while True:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/cmdline", "rb") as f:
                    if f.read().split(b"\0")[0] == b"/usr/Engine/Engine":
                        return int(entry)
            except OSError:
                pass
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.5)


def power_key_devices():
    """Open every input device that has a power key."""
    devices = []
    for name in sorted(os.listdir("/sys/class/input")):
        if not name.startswith("event"):
            continue
        try:
            with open(f"/sys/class/input/{name}/device/capabilities/key") as f:
                # Hex words, most significant first.
                keys = int("".join(word.rjust(16, "0") for word in f.read().split()), 16)
            if keys >> KEY_POWER & 1:
                devices.append(os.open(f"/dev/input/{name}", os.O_RDONLY | os.O_NONBLOCK))
                log(f"watching /dev/input/{name}")
        except (OSError, ValueError):
            pass
    return devices


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/srv/engine"
    reason_path = os.path.join(root, "tmp/engine-quit-reason")

    pid = find_engine(30)
    if pid is None:
        log("no Engine process to watch")
        return 1
    engine = os.pidfd_open(pid)
    devices = power_key_devices()

    def prompt():
        """Identity of the reason file, which exists while the prompt is up."""
        try:
            return os.stat(reason_path).st_ino
        except OSError:
            return None

    seen_prompt = None      # the prompt the last press found
    last_unanswered = None  # when a press last found no prompt at all

    while True:
        readable, _, _ = select.select([engine] + devices, [], [])
        if engine in readable:
            try:
                with open(reason_path) as f:
                    reason = f.read().strip()
            except OSError:
                reason = ""
            log(f"Engine exited, reason: {reason or 'none'}")
            if reason == "Poweroff":
                subprocess.run(["systemctl", "poweroff"])
            elif reason == "Reboot":
                subprocess.run(["systemctl", "reboot"])
            return 0

        pressed = False
        for fd in readable:
            try:
                data = os.read(fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            except OSError:
                devices.remove(fd)
                continue
            for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, kind, code, value = EVENT.unpack_from(data, offset)
                if kind == EV_KEY and code == KEY_POWER and value == 1:
                    pressed = True
        if not pressed:
            continue

        time.sleep(SETTLE)
        now = time.monotonic()
        found = prompt()
        if found is not None and found == seen_prompt:
            log("power button pressed with the prompt up: powering off")
            subprocess.run(["systemctl", "poweroff"])
        elif found is None and last_unanswered is not None and now - last_unanswered < UNANSWERED_WINDOW:
            log("power button pressed twice, unanswered by Engine: powering off")
            subprocess.run(["systemctl", "poweroff"])
        else:
            log("power button pressed: " + ("Engine is asking" if found is not None else "no prompt"))
        seen_prompt = found
        last_unanswered = now if found is None else None


if __name__ == "__main__":
    sys.exit(main())
