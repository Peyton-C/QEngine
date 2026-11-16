#!/usr/bin/env python3
"""Summarise a QEMU wav-backend capture: is there sound, and does it drop out?

  wavcheck.py file.wav [--tone] [--last SECONDS]

Stdlib only. Splits the left channel into 10 ms windows, calls a window silent
when its peak is under 0.1% of full scale, and reports the gaps (silent runs
with sound on both sides). With --tone it also estimates the frequency per
second from zero crossings, which shows pitch error and drift. With --last it
reads only the end of the file, so a capture can be checked while it grows.
"""
import sys, wave, array, os

path = sys.argv[1]; tone = "--tone" in sys.argv
tail_s = float(sys.argv[sys.argv.index("--last") + 1]) if "--last" in sys.argv else None
size = os.path.getsize(path)
# QEMU writes the header's sizes only on a clean exit, and Python's wave module
# refuses the placeholder, so read the three fields by hand.
import struct
with open(path, "rb") as h:
    hdr = h.read(44)
ch, rate = struct.unpack_from("<HI", hdr, 22)
sw = struct.unpack_from("<H", hdr, 34)[0] // 8
nframes = (size - 44) // (ch * sw)
skip = max(0, nframes - int(tail_s * rate)) if tail_s else 0
f = open(path, "rb"); f.seek(44 + skip * ch * sw); raw = f.read((nframes - skip) * ch * sw)
code = {2: "h", 4: "i"}[sw]
a = array.array(code); a.frombytes(raw)
left = a[0::ch]
full = float(1 << (8 * sw - 1))
win = rate // 100
peaks = [max(abs(min(left[i:i + win])), max(left[i:i + win])) / full
         for i in range(0, len(left) - win, win)]
print(f"{path}: {ch}ch {sw*8}bit {rate}Hz, {len(left)/rate:.1f}s, peak {max(peaks, default=0):.3f}")
loud = [p > 0.001 for p in peaks]
if not any(loud):
    print("  silent throughout"); sys.exit(1)
first, last = loud.index(True), len(loud) - 1 - loud[::-1].index(True)
gaps = []; i = first
while i <= last:
    if not loud[i]:
        j = i
        while not loud[j]: j += 1
        gaps.append((i, j - i)); i = j
    else:
        i += 1
active = (last - first + 1) / 100
print(f"  sound from {first/100:.2f}s to {last/100:.2f}s ({active:.1f}s)")
print(f"  gaps: {len(gaps)}, total {sum(g for _, g in gaps)*10} ms, longest {max((g for _, g in gaps), default=0)*10} ms")
for s, g in gaps[:12]:
    print(f"    at {s/100:.2f}s for {g*10} ms")
if tone:
    freqs = []
    for sec in range(first // 100 + 1, last // 100):
        seg = left[sec * rate:(sec + 1) * rate]
        zc = sum(1 for k in range(1, len(seg)) if seg[k - 1] < 0 <= seg[k])
        freqs.append(zc)
    if freqs:
        print(f"  tone: min {min(freqs)} Hz, max {max(freqs)} Hz, mean {sum(freqs)/len(freqs):.1f} Hz over {len(freqs)} s")
    # Clicks too short for the 10 ms windows: a steady tone crosses zero at a fixed
    # interval, so any other interval marks a discontinuity (a dropped or repeated
    # block). Sub-sample jitter moves an interval by one sample at most.
    lo, hi = first * win + rate // 10, (last + 1) * win - rate // 10
    prev = None; ivals = {}; bad = []
    for k in range(lo + 1, hi):
        if left[k - 1] < 0 <= left[k]:
            if prev is not None:
                d = k - prev; ivals[d] = ivals.get(d, 0) + 1
            prev = k
    if ivals:
        mode = max(ivals, key=ivals.get)
        odd = {d: n for d, n in ivals.items() if abs(d - mode) > 1}
        print(f"  discontinuities: {sum(odd.values())} (period {mode} samples; odd intervals {dict(sorted(odd.items())[:8])})")
