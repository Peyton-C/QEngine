#!/usr/bin/env python3
"""
Drive the DDJ-FLX10 jog screens with fake deck data, for working on the screen
protocol without Engine. Same transport as flx10_bridge.py: unlock over ep0,
the rekordbox init replay and keepalive over rawmidi, packets over hidraw.

Stop flx10_bridge.py first; the rawmidi node opens exclusively.

  sudo python3 flx10_screen_test.py --wave --pos          synthetic 128/174.5 BPM decks
  sudo python3 flx10_screen_test.py --wave --pos --wave-file <track>
                                                          a real track's waveform
  --loop-test      toggle the loop visual on deck 1 every 10 s
  --legacy-pos     the relay daemon's BE24 position, which jitters
  --bpm 1=174.5    override a deck's fake BPM
"""
import argparse, fcntl, glob, json, os, struct, sys, time

VID, PID = 0x2B73, 0x0041
VENDOR_CMDS = [
    (0x0100, 0xC028), (0x0000, 0xC029), (0x0200, 0xC013), (0x0000, 0xC02B),
    (0x0100, 0xC026), (0x0000, 0xC01D), (0x0100, 0xC027),
]
DECK_BYTES = {1: 0x10, 2: 0x20, 3: 0x30, 4: 0x40}
SERATO_DECK_31 = {0x10: 0x02, 0x20: 0x01, 0x30: 0x04, 0x40: 0x03}
POS_RATE = 128.0
HERE = os.path.dirname(os.path.abspath(__file__))


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def usbmidi_decode(data):
    """USB-MIDI 4-byte event packets -> raw MIDI messages."""
    msgs, sysex = [], bytearray()
    for j in range(0, len(data) - 3, 4):
        ev = data[j:j + 4]
        cin = ev[0] & 0x0F
        if 0x8 <= cin <= 0xE:
            n = 2 if (ev[1] & 0xF0) in (0xC0, 0xD0) else 3
            msgs.append(bytes(ev[1:1 + n]))
        elif cin == 0x4:
            sysex += ev[1:4]
        elif cin in (0x5, 0x6, 0x7):
            sysex += ev[1:cin - 3]
            msgs.append(bytes(sysex)); sysex = bytearray()
        elif cin == 0xF:
            msgs.append(bytes(ev[1:2]))
    return msgs


# The relay daemon's keepalive is a USB-MIDI frame; this is the SysEx inside it.
KEEPALIVE = usbmidi_decode(bytes.fromhex("04f000400405000004040100075000f7"))[0]


def find_device():
    for vf in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        d = os.path.dirname(vf)
        try:
            v = int(open(vf).read(), 16)
            p = int(open(f"{d}/idProduct").read(), 16)
        except (OSError, ValueError):
            continue
        if (v, p) == (VID, PID):
            return d
    return None


def usb_unlock(sysfs):
    bus = int(open(f"{sysfs}/busnum").read())
    dev = int(open(f"{sysfs}/devnum").read())
    node = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
    # struct usbdevfs_ctrltransfer { u8 type, req; u16 value, index, length;
    #                                u32 timeout; void *data; }
    fmt = "BBHHHIP"
    size = struct.calcsize(fmt)
    USBDEVFS_CONTROL = (3 << 30) | (size << 16) | (ord("U") << 8) | 0
    fd = os.open(node, os.O_RDWR)
    try:
        for i, (wv, wi) in enumerate(VENDOR_CMDS, 1):
            buf = struct.pack(fmt, 0x40, 3, wv, wi, 0, 1000, 0)
            try:
                fcntl.ioctl(fd, USBDEVFS_CONTROL, buf)
                log(f"  unlock [{i}/7] ok")
            except OSError as e:
                log(f"  unlock [{i}/7] FAIL {e}")
            time.sleep(0.005)
    finally:
        os.close(fd)


def find_hidraw(sysfs):
    hits = sorted(glob.glob(f"{sysfs}:1.5/*/hidraw/hidraw*"))
    return "/dev/" + os.path.basename(hits[0]) if hits else None


def find_rawmidi(sysfs):
    cards = glob.glob(f"{sysfs}:*/sound/card*")
    if not cards:
        return None
    n = os.path.basename(cards[0])[4:]
    node = f"/dev/snd/midiC{n}D0"
    return node if os.path.exists(node) else None


def build_xx27_v5(deck_byte, bpm, pos, duration, with_pos, key_byte=0x82,
                  tempo_pct=0.0, b27=0):
    """Veezuhz's final Serato-mode xx 27 (PioneerDDJFLX10-screen.js v5,
    2026-05-29): b5/b6 = elapsed min/sec, b7/b8 = sub-second ms (0..999),
    b21/b22 = position counters kept in phase with b6, beat-grid mode bytes."""
    p = bytearray(128)
    p[0], p[1], p[2], p[3], p[4] = deck_byte, 0x27, 0xB4, 0x88, 0x01
    p[19], p[20], p[25], p[30] = 0x07, 0x0E, 0x80, 0x0D
    p[31] = SERATO_DECK_31[deck_byte]
    p[32], p[33], p[34] = 0xAD, 0x05, 0x00
    p[29] = key_byte                       # Camelot; 0x82 = 8A
    t_enc = int(round(tempo_pct * 100)) & 0xFFFF   # LE16 two's complement
    p[16], p[17] = t_enc & 0xFF, t_enc >> 8
    p[27] = b27                            # non-zero = beat-loop visual
    total_ms = int(duration * 1000)
    p[9] = total_ms // 60000
    p[10] = (total_ms % 60000) // 1000
    ms = (total_ms % 1000) // 100 * 100
    p[11], p[12] = ms & 0xFF, ms >> 8
    p[13] = int(bpm) & 0xFF
    # BPM decimals: b14 high nibble = tenths, b15 = hundredths (seen on
    # hardware: b14=0x80 b15=0x01 displayed .81).
    hundredths = int(round((bpm - int(bpm)) * 100))
    p[14] = (hundredths // 10) << 4
    p[15] = hundredths % 10
    if with_pos:
        t = pos * duration
        sec = int(t)
        sub = t - sec
        sub_ms = min(999, int(sub * 1000))
        ticks = sec * 1024 + min(1023, int(sub * 1024))
        p[5], p[6] = (sec // 60) & 0xFF, sec % 60
        p[7], p[8] = sub_ms & 0xFF, (sub_ms >> 8) & 0x03
        p[21] = (ticks * 2) & 0xFF
        if SERATO_DECK_31[deck_byte] in (0x01, 0x02):
            p[22] = int(ticks / 123.2) % 15
    return bytes(p)


def build_xx27(deck_byte, bpm, pos, duration, with_pos):
    p = bytearray(128)
    p[0], p[1], p[2], p[3], p[4] = deck_byte, 0x27, 0xB4, 0x80, 0x01
    p[20], p[25], p[30] = 0x0E, 0x80, 0x0D
    p[31] = SERATO_DECK_31[deck_byte]
    p[32], p[33], p[34] = 0xE0, 0x01, 0x00
    remaining = max(0.0, duration * (1.0 - pos))
    total_ms = int(round(remaining * 1000))
    p[9] = total_ms // 60000
    p[10] = (total_ms % 60000) // 1000
    ms = total_ms % 1000
    p[11], p[12] = ms & 0xFF, ms >> 8
    p[29] = 0x92
    p[13] = int(bpm) & 0xFF
    p[14] = (int(round((bpm - int(bpm)) * 10)) & 0x0F) << 4
    if with_pos:
        v = min(0xFFFFFF, int(pos * duration * POS_RATE))
        p[5], p[6], p[7] = v >> 16, (v >> 8) & 0xFF, v & 0xFF
    return bytes(p)


# --- rekordbox waveform family (Loui1979 FINDINGS_rekordbox_jogscreen.md) ---
# 6-byte header [deck, type, LE16 seq (1-based), LE16 total], then 122 bytes of
# a continuous 3-byte column stream that straddles packets.
FULL_FPS = 150.0          # 0x38 columns per second
OVERVIEW_DECIM = 21       # 0x37 = 0x38 decimated
OVERVIEW_SUBHDR = bytes([0x80, 0x01, 0x01, 0x00])


def packetise(deck_byte, ptype, body):
    total = max(1, (len(body) + 121) // 122)
    out = []
    for i in range(total):
        p = bytearray(128)
        p[0], p[1] = deck_byte, ptype
        p[2], p[3] = (i + 1) & 0xFF, (i + 1) >> 8
        p[4], p[5] = total & 0xFF, total >> 8
        seg = body[i * 122:(i + 1) * 122]
        p[6:6 + len(seg)] = seg
        out.append(bytes(p))
    return out


def synth_columns(n, bpm, order):
    """Fake track, one (low, mid, high) per column: kick on every beat, hats on
    the off-beats, mid stepping up every 16 bars, and silence for bars 33-40 so
    scroll direction and speed are obvious. Packed in `order` (lane -> band)."""
    cols = bytearray(3 * n)
    spb = 60.0 / bpm
    idx = {"low": 0, "mid": 1, "high": 2}
    lanes = [idx[b] for b in order]
    for c in range(n):
        t = c / FULL_FPS
        beat = t / spb
        bar = int(beat // 4)
        ph = beat % 1.0
        if 32 <= bar < 40:
            bands = (0, 0, 0)
        else:
            low = int(0xB0 * max(0.0, 1.0 - ph * 4))            # kick decay
            high = 0x90 if 0.45 < ph < 0.6 else 0x10              # off-beat hat
            mid = 0x20 + 0x18 * ((bar // 16) % 5)
            bands = (low, mid, high)
        for lane in range(3):
            cols[c * 3 + lane] = bands[lanes[lane]]
    return bytes(cols)


def build_xx30(deck_byte, duration):
    p = bytearray(128)
    p[0], p[1], p[2], p[4] = deck_byte, 0x30, 0x01, 0x01
    total_ms = int(round(duration * 1000))
    p[6] = total_ms // 60000
    p[7] = (total_ms % 60000) // 1000
    ms = total_ms % 1000
    p[8], p[9] = ms & 0xFF, ms >> 8
    return bytes(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wave", action="store_true",
                    help="upload a synthetic 0x37/0x38 waveform per deck first")
    ap.add_argument("--wave-file", default="",
                    help="build the waveform from this audio file with the bridge's ffmpeg code")
    ap.add_argument("--band-order", default="high,low,mid",
                    help="lane order of each 3-byte column")
    ap.add_argument("--duration", type=float, default=163.0)
    ap.add_argument("--loop-test", action="store_true",
                    help="toggle byte 27 (loop visual) on deck 1 every 10 s")
    ap.add_argument("--legacy-pos", action="store_true",
                    help="relay-daemon xx27 (BE24 elapsed x 128) instead of v5")
    ap.add_argument("--interval", type=float, default=0.01,
                    help="seconds between xx27 rounds")
    ap.add_argument("--bpm", default="",
                    help="override fake BPMs, e.g. 1=174.5,2=128")
    ap.add_argument("--pos", action="store_true",
                    help="also send playhead position bytes [5..7]")
    ap.add_argument("--decks", default="1,2,3,4")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--no-unlock", action="store_true")
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("need root")

    sysfs = find_device()
    if not sysfs:
        sys.exit("FLX10 (2b73:0041) not found")
    hid, midi = find_hidraw(sysfs), find_rawmidi(sysfs)
    log(f"device {sysfs}  hidraw={hid}  rawmidi={midi}")
    if not hid or not midi:
        sys.exit("missing hidraw (interface 5) or rawmidi node -- replug and retry")

    if not args.no_unlock:
        log("ep0 vendor unlock")
        usb_unlock(sysfs)
        time.sleep(0.3)

    mfd = os.open(midi, os.O_WRONLY)
    hfd = os.open(hid, os.O_RDWR)

    seq = json.load(open(os.path.join(HERE, "rb_init_sequence.json")))
    log(f"replaying {len(seq)} init frames")
    for ep, hexb in seq:
        data = bytes.fromhex(hexb)
        if ep == "ep3":
            for m in usbmidi_decode(data):
                os.write(mfd, m)
        else:
            os.write(hfd, b"\x00" + data)
        time.sleep(0.002)

    decks = [int(d) for d in args.decks.split(",")]
    fake = {1: 128.0, 2: 174.5, 3: 90.0, 4: 140.2}
    keys = {1: 0x82, 2: 0x8F, 3: 0x98, 4: 0x89}       # 8A, 9B, 1A, 12B
    tempo = {1: 5.0, 2: -3.25, 3: 0.0, 4: 1.5}
    for kv in filter(None, args.bpm.split(",")):
        k, v = kv.split("=")
        fake[int(k)] = float(v)
    duration = args.duration
    last_ka = time.monotonic()

    def keepalive():
        nonlocal last_ka
        if time.monotonic() - last_ka >= 0.2:
            os.write(mfd, KEEPALIVE)
            last_ka = time.monotonic()

    if args.wave:
        order = args.band_order.split(",")
        n_full = int(round(duration * FULL_FPS))
        for d in decks:
            db = DECK_BYTES[d]
            if args.wave_file:
                sys.path.insert(0, HERE)
                import flx10_bridge as B
                full = B.columns(B.envelopes(args.wave_file), n_full)
            else:
                full = synth_columns(n_full, fake[d], order)
            # Overview: max-pool the full stream per lane.
            n_ov = max(1, n_full // OVERVIEW_DECIM)
            ov = bytearray(3 * n_ov)
            for o in range(n_ov):
                for lane in range(3):
                    ov[o * 3 + lane] = max(full[(o * OVERVIEW_DECIM + k) * 3 + lane]
                                           for k in range(OVERVIEW_DECIM))
            pkts = ([build_xx30(db, duration)]
                    + packetise(db, 0x37, OVERVIEW_SUBHDR + bytes(ov))
                    + packetise(db, 0x38, full))
            log(f"deck {d}: uploading {len(pkts)} packets "
                f"({n_ov} overview + {n_full} full columns, order {order})")
            for p in pkts:
                os.write(hfd, b"\x00" + p)
                keepalive()
                time.sleep(0.002)

    log(f"live: decks {decks}, pos bytes {'on' if args.pos else 'off'}, "
        f"{args.seconds:.0f}s -- watch the jog screens (Ctrl-C to stop)")
    t0 = time.monotonic()
    loop_on = None
    try:
        while (now := time.monotonic()) - t0 < args.seconds:
            keepalive()
            pos = ((now - t0) / duration) % 1.0
            for d in decks:
                if args.legacy_pos:
                    pkt = build_xx27(DECK_BYTES[d], fake[d], pos, duration, args.pos)
                else:
                    b27 = 0
                    if args.loop_test and d == 1:
                        on = int((now - t0) // 10) % 2 == 1
                        if on != loop_on:
                            loop_on = on
                            log(f"deck 1 loop visual {'ON (b27=0x1a)' if on else 'off'}")
                        b27 = 0x1A if on else 0
                    pkt = build_xx27_v5(DECK_BYTES[d], fake[d], pos, duration,
                                        args.pos, keys[d], tempo[d], b27)
                os.write(hfd, b"\x00" + pkt)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(hfd)
        os.close(mfd)
    log("done")


if __name__ == "__main__":
    main()
