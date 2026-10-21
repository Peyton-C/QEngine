#!/usr/bin/env python3
"""
Try sending a track's artwork to the DDJ-FLX10's jog screens, alongside the
running bridge (hidraw takes more than one writer).

VirtualDJ was seen sending two JPEGs per deck, 0x33 (~3 KB) and 0x2B (~1 KB).
The packet layout is a guess from the waveform packets: [deck, type, LE16 seq
from 1, LE16 total], then data. --sub 3 puts "<LE16 size> 00" in front of the
JPEG, as Veezuhz decoded Serato's 0x33 album art; --sub 4 the D1 background
upload's "00 <LE16 size> 00".

  sudo python3 flx10_art_test.py <track> --type 0x33 --size 240 [--deck 1] [--sub 3]
"""
import argparse, glob, os, subprocess, sys, time

DECK_BYTES = {1: 0x10, 2: 0x20, 3: 0x30, 4: 0x40}


def find_hidraw():
    for path in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            uevent = open(os.path.join(path, "device", "uevent")).read().upper()
        except OSError:
            continue
        if "2B73" in uevent and "0041" in uevent:
            return "/dev/" + os.path.basename(path)
    return None


def artwork_jpeg(track, size, quality):
    """The track's embedded cover, square, as a baseline 4:2:0 JPEG."""
    return subprocess.run(
        ["ffmpeg", "-v", "error", "-nostdin", "-i", track, "-map", "0:v:0", "-frames:v", "1",
         "-vf", f"scale={size}:{size}", "-pix_fmt", "yuvj420p", "-q:v", str(quality),
         "-f", "mjpeg", "-"],
        stdout=subprocess.PIPE, check=True).stdout


def packets(deck_byte, ptype, body, header=6):
    """header 6: [deck, type, LE16 seq, LE16 total] as the waveform packets.
    header 5: [deck, type, seq, 0, total] as Veezuhz wrote the JPEG slots."""
    room = 128 - header
    total = max(1, (len(body) + room - 1) // room)
    for i in range(total):
        p = bytearray(128)
        p[0], p[1] = deck_byte, ptype
        if header == 6:
            p[2], p[3] = (i + 1) & 0xFF, (i + 1) >> 8
            p[4], p[5] = total & 0xFF, total >> 8
        else:
            p[2], p[4] = (i + 1) & 0xFF, total & 0xFF
        seg = body[i * room:(i + 1) * room]
        p[header:header + len(seg)] = seg
        yield bytes(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track")
    ap.add_argument("--type", type=lambda s: int(s, 0), default=0x33)
    ap.add_argument("--size", type=int, default=240)
    ap.add_argument("--quality", type=int, default=6, help="ffmpeg -q:v, 2 best .. 31 worst")
    ap.add_argument("--deck", type=int, default=1)
    ap.add_argument("--sub", type=int, choices=(0, 3, 4), default=3)
    ap.add_argument("--header", type=int, choices=(5, 6), default=6)
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("need root")

    jpeg = artwork_jpeg(args.track, args.size, args.quality)
    body = jpeg
    size = bytes([len(jpeg) & 0xFF, len(jpeg) >> 8])
    if args.sub == 3:
        body = size + b"\x00" + jpeg
    elif args.sub == 4:
        body = b"\x00" + size + b"\x00" + jpeg
    pkts = list(packets(DECK_BYTES[args.deck], args.type, body, args.header))
    print(f"{len(jpeg)} byte JPEG, {args.size}px, as {len(pkts)} x 0x{args.type:02x} to deck {args.deck}"
          f", {args.sub}-byte sub-header, {args.header}-byte header")

    fd = os.open(find_hidraw(), os.O_RDWR)
    for p in pkts:
        os.write(fd, b"\x00" + p)
        time.sleep(0.002)
    os.close(fd)
    print("sent")


if __name__ == "__main__":
    main()
