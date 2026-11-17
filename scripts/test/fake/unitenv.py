#!/usr/bin/env python3
"""unitenv.py < unit text : print a unit's Environment= settings as sh exports.

The input is a service unit followed by its drop-ins, in the order systemd
applies them, which is what `systemctl cat` prints. The fake Engine has to run
with exactly what the builder gave the real one, LD_PRELOAD above all, and the
unit files are the only place that is written down."""
import shlex
import sys

env = {}
section = ""
for raw in sys.stdin:
    line = raw.strip()
    if line.startswith("["):
        section = line
    if section != "[Service]" or not line.startswith("Environment="):
        continue
    value = line[len("Environment="):]
    if not value:
        env.clear()  # an empty assignment resets the list
        continue
    # systemd quotes a whole assignment, "A=b c", where it holds a space.
    for item in shlex.split(value):
        if "=" in item:
            key, val = item.split("=", 1)
            env[key] = val
for key, val in env.items():
    print("export %s=%s" % (key, shlex.quote(val)))
