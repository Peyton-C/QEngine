#!/bin/bash
# make_tones.sh [MINUTES] : write $OUT/usb.img, a FAT32 stick holding
# "Tone 1000 Hz.wav" and "Tone 440 Hz.wav", each MINUTES long (default 5).
# Use a long one, such as 35, for a soak run, and pass that image to attach.sh.
#
# A steady sine makes every fault measurable: wavcheck.py reads a dropout as a
# gap, a dropped or repeated block as an odd zero-crossing interval, and a
# sample-rate error as the wrong pitch. Music hides all three. 44.1 kHz, so the
# file is never resampled on the way in and only Engine's output rate matters.
#
# macOS only, as it is the build host: hdiutil makes the image, with an MBR so
# the guest's losetup -P finds a partition the way edisksd expects of a stick.
set -eu
source "$(dirname "$0")/common.sh"
minutes="${1:-5}"
work="$OUT/tones"; mkdir -p "$work"
for f in 1000 440; do
    python3 - "$work/Tone $f Hz.wav" "$f" "$minutes" <<'PY'
import math, struct, sys, wave
path, freq, minutes = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
rate = 44100
period = [int(0.5 * 32767 * math.sin(2 * math.pi * freq * i / rate)) for i in range(rate)]
second = b"".join(struct.pack("<hh", s, s) for s in period)
with wave.open(path, "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    for _ in range(int(minutes * 60)):
        w.writeframes(second)
PY
done
size_mb=$(( $(du -sm "$work" | cut -f1) + 32 ))
dmg="$OUT/tones.dmg"
hdiutil create -quiet -ov -size "${size_mb}m" -fs "MS-DOS FAT32" -volname QTEST -layout MBRSPUD "$dmg"
mnt=$(hdiutil attach "$dmg" 2>/dev/null | awk -F'\t' '/Volumes/{print $NF}')
cp "$work"/*.wav "$mnt/"
hdiutil detach -quiet "$mnt"
hdiutil convert -quiet "$dmg" -format UDTO -ov -o "$OUT/usb"
mv -f "$OUT/usb.cdr" "$OUT/usb.img"
rm -f "$dmg"
echo "$OUT/usb.img: ${size_mb} MB, $minutes min per tone"
