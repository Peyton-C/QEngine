#!/usr/bin/env python3
"""Run one shell command on a guest's serial console (QEMU -serial tcp:...,server,nowait).

  ser.py PORT 'command' [timeout_s]

Logs in as root if a login prompt is showing, sends the command wrapped in
markers and prints what came back between them.
"""
import socket, sys, time, re

port = int(sys.argv[1]); cmd = sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 30
s = socket.create_connection(("127.0.0.1", port)); s.settimeout(0.5)

def read(until=None, t=3.0):
    buf = b""; end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            buf += d
            if until and until in buf: break
        except socket.timeout:
            if until is None and buf: break
    return buf

s.sendall(b"\n"); out = read(t=2)
if b"login:" in out.splitlines()[-1] if out.strip() else False:
    s.sendall(b"root\n"); out = read(t=4)
    if b"assword" in out:
        s.sendall(b"\n"); read(t=3)
tag = "QEM%dK" % int(time.time())
# The tag is split in the command line so the echo of the command itself never matches.
s.sendall(("echo %s''BEGIN; %s; echo %s''END $?\n" % (tag, cmd, tag)).encode())
out = read(until=(tag + "END").encode(), t=timeout) + read(t=0.6)
txt = out.decode("utf-8", "replace").replace("\r", "")
m = re.search(re.escape(tag) + r"BEGIN\n(.*?)" + re.escape(tag) + r"END (\d+)", txt, re.S)
if m:
    sys.stdout.write(m.group(1)); sys.exit(int(m.group(2)))
sys.stdout.write(txt); sys.exit(124)
