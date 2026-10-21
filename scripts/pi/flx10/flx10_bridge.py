#!/usr/bin/env python3
"""
flx10_bridge.py -- Engine's decks on the Pioneer DDJ-FLX10's jog screens.

Engine side: the MONTERO mod in shims/montero/mods/flx10 sends every deck's
state as JSON, ten times a second, to the abstract socket @montero/flx10.

Controller side: the screen protocol reverse-engineered by Veezuhz and
Loui1979 (README.md has the credits), spoken through the kernel's own drivers
so midisurface can use the same controller at the same time -- nothing is
unbound or claimed:

  ep0 vendor unlock      seven control transfers (USBDEVFS_CONTROL)
  rekordbox init replay  rb_init_sequence.json, through the ALSA sequencer
  keepalive SysEx        every 200 ms, sequencer
  xx 27 deck state       every 10 ms for each loaded deck, hidraw
  waveform               xx 30 length, 0x37 overview, 0x38 full, on track load

The waveform comes from the track file itself -- Engine's library keeps only a
coarse overview -- decoded by ffmpeg into low/mid/high envelopes.

Run as root on the Pi host, with Engine started with montero.so preloaded and
the flx10 mod in /data/mods. Stdlib and ffmpeg only.
"""
import argparse, array, fcntl, glob, json, os, re, signal, socket, struct
import subprocess, sys, threading, time

VID, PID = 0x2B73, 0x0041
VENDOR_CMDS = [
    (0x0100, 0xC028), (0x0000, 0xC029), (0x0200, 0xC013), (0x0000, 0xC02B),
    (0x0100, 0xC026), (0x0000, 0xC01D), (0x0100, 0xC027),
]
DECK_BYTES = {1: 0x10, 2: 0x20, 3: 0x30, 4: 0x40}
# b31: decks 1/2 are the primary pair, 3/4 the secondary.
STATE_BYTE_31 = {0x10: 0x02, 0x20: 0x01, 0x30: 0x04, 0x40: 0x03}

FULL_FPS = 150.0                 # 0x38 columns per second of track
OVERVIEW_COLS = 1186             # 0x37 width, as rekordbox sent it
OVERVIEW_SUBHDR = bytes([0x80, 0x01, 0x01, 0x00])
ENVELOPE_RATE = 4500             # ffmpeg output rate; 30 samples per column
BAND_PEAK = (0xB0, 0xA0, 0x90)   # low, mid, high: what each band's loud end maps to
LANES = (2, 0, 1)                # column bytes are [high, low, mid]

TICK = 0.01                      # xx 27 period
WAVE_PACKETS_PER_TICK = 4        # waveform upload rate, between the pings

HERE = os.path.dirname(os.path.abspath(__file__))


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


# ---- device ------------------------------------------------------------------

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


KEEPALIVE = usbmidi_decode(bytes.fromhex("04f000400405000004040100075000f7"))[0]


def find_device():
    for vf in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        d = os.path.dirname(vf)
        try:
            if (int(open(vf).read(), 16), int(open(f"{d}/idProduct").read(), 16)) == (VID, PID):
                return d
        except (OSError, ValueError):
            continue
    return None


def usb_unlock(sysfs):
    bus = int(open(f"{sysfs}/busnum").read())
    dev = int(open(f"{sysfs}/devnum").read())
    # struct usbdevfs_ctrltransfer { u8 type, req; u16 value, index, length;
    #                                u32 timeout; void *data; }
    fmt = "BBHHHIP"
    request = (3 << 30) | (struct.calcsize(fmt) << 16) | (ord("U") << 8) | 0
    fd = os.open(f"/dev/bus/usb/{bus:03d}/{dev:03d}", os.O_RDWR)
    try:
        for wv, wi in VENDOR_CMDS:
            fcntl.ioctl(fd, request, struct.pack(fmt, 0x40, 3, wv, wi, 0, 1000, 0))
            time.sleep(0.005)
    finally:
        os.close(fd)


def find_hidraw(sysfs):
    hid = sorted(glob.glob(f"{sysfs}:1.5/*/hidraw/hidraw*"))
    return "/dev/" + os.path.basename(hid[0]) if hid else None


def find_seq_client(name="DDJ-FLX10"):
    """The controller's ALSA sequencer client, from /proc/asound/seq/clients."""
    try:
        for line in open("/proc/asound/seq/clients"):
            m = re.match(r'Client\s+(\d+)\s*:\s*"([^"]*)"', line)
            if m and name in m[2]:
                return int(m[1])
    except OSError:
        pass
    return None


class SeqOut:
    """MIDI to the controller through the ALSA sequencer, by ctypes.

    Not the rawmidi node: midisurface reaches the same controller through the
    sequencer, and the kernel opens a USB MIDI output for the sequencer only
    when nobody holds it as rawmidi. With the bridge on /dev/snd/midiC0D0,
    midisurface's lamp port failed to connect ("could not send to 16:0")."""

    EVENT_SIZE = 28            # sizeof(snd_seq_event_t), on every ABI
    SUBSCRIBERS, DIRECT = 254, 253

    def __init__(self, client, port=0):
        import ctypes
        self.ct = ctypes
        self.lib = ctypes.CDLL("libasound.so.2")
        self.seq = ctypes.c_void_p()
        if self.lib.snd_seq_open(ctypes.byref(self.seq), b"default", 1, 0) < 0:   # OUTPUT
            raise OSError("snd_seq_open failed")
        self.lib.snd_seq_set_client_name(self.seq, b"flx10_bridge")
        self.port = self.lib.snd_seq_create_simple_port(self.seq, b"screens", 0, 1 << 20)
        if self.port < 0 or self.lib.snd_seq_connect_to(self.seq, self.port, client, port) < 0:
            self.close()
            raise OSError(f"cannot send to sequencer client {client}:{port}")
        self.enc = ctypes.c_void_p()
        self.lib.snd_midi_event_new(256, ctypes.byref(self.enc))
        self.ev = ctypes.create_string_buffer(self.EVENT_SIZE)

    def send(self, msg):
        ct = self.ct
        self.lib.snd_midi_event_reset_encode(self.enc)
        ct.memset(self.ev, 0, self.EVENT_SIZE)
        if self.lib.snd_midi_event_encode(self.enc, msg, len(msg), self.ev) <= 0:
            return
        raw = ct.cast(self.ev, ct.POINTER(ct.c_ubyte))
        raw[3] = self.DIRECT                       # queue
        raw[13] = self.port                        # source.port
        raw[14], raw[15] = self.SUBSCRIBERS, self.DIRECT   # dest: subscribers
        if self.lib.snd_seq_event_output_direct(self.seq, self.ev) < 0:
            raise OSError("sequencer output failed")

    def close(self):
        if self.seq:
            self.lib.snd_seq_close(self.seq)
            self.seq = None


class Controller:
    def __init__(self, sysfs, hid, client):
        usb_unlock(sysfs)
        time.sleep(0.3)
        self.midi = SeqOut(client)
        self.hfd = os.open(hid, os.O_RDWR)
        self.last_keepalive = 0.0
        for ep, hexb in json.load(open(os.path.join(HERE, "rb_init_sequence.json"))):
            data = bytes.fromhex(hexb)
            if ep == "ep3":
                for m in usbmidi_decode(data):
                    self.midi.send(m)
            else:
                self.screen(data)
            time.sleep(0.002)

    def screen(self, pkt):
        os.write(self.hfd, b"\x00" + pkt)       # hidraw wants a report id

    def keepalive(self, now):
        if now - self.last_keepalive >= 0.2:
            self.midi.send(KEEPALIVE)
            self.last_keepalive = now

    def close(self):
        try:
            os.close(self.hfd)
        except OSError:
            pass
        self.midi.close()


# ---- packets -------------------------------------------------------------------

def camelot_byte(key):
    """'8A' -> 0x82. Each +2 steps the Camelot number by +7; A even, B odd."""
    m = re.fullmatch(r"(\d{1,2})([AB])", key or "")
    if not m:
        return 0x80                             # no key
    for i in range(12):
        if (7 + 7 * i) % 12 + 1 == int(m[1]):
            return (0x82 if m[2] == "A" else 0x81) + 2 * i
    return 0x80


def build_xx27(deck_byte, bpm, wall_sec, wall_dur, tempo_pct, key_byte, loop):
    """Veezuhz's final xx 27 (screen.js v5), with the BPM decimals corrected:
    b5/b6 elapsed min/sec, b7/b8 sub-second ms (0..999), b9..b12 duration,
    b13 BPM, b14 high nibble tenths, b15 hundredths, b16/b17 tempo % x100,
    b21/b22 position counters kept in phase with b6, b27 loop, b29 key."""
    p = bytearray(128)
    p[0], p[1], p[2], p[3], p[4] = deck_byte, 0x27, 0xB4, 0x88, 0x01
    p[19], p[20], p[25], p[30] = 0x07, 0x0E, 0x80, 0x0D
    p[31] = STATE_BYTE_31[deck_byte]
    p[32], p[33], p[34] = 0xAD, 0x05, 0x00
    p[29] = key_byte
    p[27] = 0x1A if loop else 0

    total_ms = int(wall_dur * 1000)
    p[9], p[10] = total_ms // 60000, (total_ms % 60000) // 1000
    ms = (total_ms % 1000) // 100 * 100
    p[11], p[12] = ms & 0xFF, ms >> 8

    hundredths = int(round(bpm * 100))
    p[13] = (hundredths // 100) & 0xFF
    p[14] = ((hundredths % 100) // 10) << 4
    p[15] = hundredths % 10
    t_enc = int(round(tempo_pct * 100)) & 0xFFFF
    p[16], p[17] = t_enc & 0xFF, t_enc >> 8

    sec = int(wall_sec)
    sub = wall_sec - sec
    sub_ms = min(999, int(sub * 1000))
    ticks = sec * 1024 + min(1023, int(sub * 1024))
    p[5], p[6] = (sec // 60) & 0xFF, sec % 60
    p[7], p[8] = sub_ms & 0xFF, (sub_ms >> 8) & 0x03
    p[21] = (ticks * 2) & 0xFF
    if STATE_BYTE_31[deck_byte] in (0x01, 0x02):
        p[22] = int(ticks / 123.2) % 15
    return bytes(p)


def build_xx30(deck_byte, duration):
    p = bytearray(128)
    p[0], p[1], p[2], p[4] = deck_byte, 0x30, 0x01, 0x01
    total_ms = int(round(duration * 1000))
    p[6], p[7] = total_ms // 60000, (total_ms % 60000) // 1000
    ms = total_ms % 1000
    p[8], p[9] = ms & 0xFF, ms >> 8
    return bytes(p)


def packetise(deck_byte, ptype, body):
    """6-byte header [deck, type, LE16 seq from 1, LE16 total], then 122 bytes
    of a column stream that runs on across packets."""
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


# ---- waveforms -------------------------------------------------------------------

def local_path(net_path, media_root):
    """net://<uuid>/MEDIA/<rest> -> <media_root>/MEDIA/<rest>."""
    m = re.match(r"net://[^/]+/(.*)", net_path or "")
    return os.path.join(media_root, m[1]) if m else None


def envelopes(path):
    """Low/mid/high rectified envelopes at ENVELOPE_RATE, interleaved floats."""
    fmt = "aformat=sample_fmts=flt:channel_layouts=mono"
    graph = (
        "[0:a:0]pan=mono|c0=0.5*c0+0.5*c1,asplit=3[a][b][c];"
        f"[a]lowpass=f=200,lowpass=f=200,aeval=abs(val(0)),aresample={ENVELOPE_RATE},{fmt}[l];"
        f"[b]highpass=f=200,lowpass=f=2500,aeval=abs(val(0)),aresample={ENVELOPE_RATE},{fmt}[m];"
        f"[c]highpass=f=2500,highpass=f=2500,aeval=abs(val(0)),aresample={ENVELOPE_RATE},{fmt}[h];"
        "[l][m][h]amerge=inputs=3[o]")
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-filter_complex", graph,
         "-map", "[o]", "-f", "f32le", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
    env = array.array("f")
    env.frombytes(raw[:len(raw) - len(raw) % 12])
    return env


def columns(env, n_cols):
    """Max-pool the envelopes into n_cols 3-byte columns, each band scaled so
    its 99.5th percentile lands on its BAND_PEAK and clamped there -- rekordbox
    never sends more than ~0xB0.

    Every byte is at least 1. An all-zero column makes the firmware drop the
    whole upload: CRANK's waveform, which opens on 540 silent columns, never
    drew until floored, while the synthetic test pattern (a kick at column 0)
    always did."""
    frames = len(env) // 3
    bands = [env[b::3] for b in range(3)]
    pooled = []
    for band in bands:
        col = []
        for c in range(n_cols):
            lo = c * frames // n_cols
            hi = max(lo + 1, (c + 1) * frames // n_cols)
            col.append(max(band[lo:hi]) if lo < frames else 0.0)
        pooled.append(col)
    out = bytearray(3 * n_cols)
    for b, col in enumerate(pooled):
        ref = sorted(col)[int(len(col) * 0.995)] or 1.0
        scale = BAND_PEAK[b] / ref
        lane = LANES.index(b)
        for c, v in enumerate(col):
            out[c * 3 + lane] = max(1, min(BAND_PEAK[b], int(v * scale)))
    return bytes(out)


def waveform_packets(deck_byte, path, duration, max_cols, squeeze):
    """The firmware drops a 0x38 upload much over ~24500 columns. Over the
    cap, `squeeze` spreads the whole track across max_cols columns; otherwise
    the end is clipped. The 0x37 overview is the whole track at a fixed width."""
    n_full = max(1, int(round(duration * FULL_FPS)))
    if squeeze and max_cols:
        n_full = min(n_full, max_cols)
    full = columns(envelopes(path), n_full)
    n_ov = min(OVERVIEW_COLS, n_full)
    ov = bytearray(3 * n_ov)
    for o in range(n_ov):
        lo, hi = o * n_full // n_ov, max(o * n_full // n_ov + 1, (o + 1) * n_full // n_ov)
        for lane in range(3):
            ov[o * 3 + lane] = max(full[c * 3 + lane] for c in range(lo, hi))
    if max_cols:
        full = full[:3 * max_cols]
    return ([build_xx30(deck_byte, duration)]
            + packetise(deck_byte, 0x37, OVERVIEW_SUBHDR + bytes(ov))
            + packetise(deck_byte, 0x38, full))


# ---- Engine's decks ----------------------------------------------------------------

def field(d, key, part):
    return (d.get(key) or {}).get(part)


class Deck:
    """One deck as the mod last reported it, extrapolated between reports."""

    def __init__(self, n):
        self.n = n
        self.loaded = False
        self.path = ""
        self.last_wall = None
        # No state goes out for a deck until its waveform has: the firmware
        # ignores a waveform for a deck it is already showing a track on.
        self.ready = False

    def update(self, d, t):
        self.loaded = bool(field(d, "Track/SongLoaded", "st"))
        self.path = field(d, "Track/TrackNetworkPath", "s") or ""
        self.playing = bool(field(d, "PlayState", "st"))
        self.rate = field(d, "Track/SampleRate", "v") or 44100
        self.pos = (field(d, "Track/TrackPosition", "u") or 0) / self.rate
        self.duration = (field(d, "Track/TrackLength", "u") or 0) / self.rate
        self.bpm = field(d, "CurrentBPM", "u") or 0.0
        orig = field(d, "Track/OriginalTempo", "u") or 0.0
        self.ratio = self.bpm / orig if orig > 0 and self.bpm > 0 else 1.0
        self.key = field(d, "Track/CurrentKey", "s") or ""
        self.loop = bool(field(d, "Track/LoopEnableState", "st"))
        self.t = t

    def packet(self, now):
        file_sec = self.pos + ((now - self.t) * self.ratio if self.playing else 0.0)
        file_sec = min(max(0.0, file_sec), self.duration)
        wall = file_sec / self.ratio
        # The firmware redraws on any backwards step, so a report that lands
        # a little behind our extrapolation is held rather than followed.
        # A bigger step back is a real seek.
        if self.playing and self.last_wall is not None and self.last_wall - 0.3 < wall < self.last_wall:
            wall = self.last_wall
        self.last_wall = wall
        return build_xx27(DECK_BYTES[self.n], self.bpm, wall, self.duration / self.ratio,
                          (self.ratio - 1.0) * 100.0, camelot_byte(self.key), self.loop)


class Bridge:
    def __init__(self, args):
        self.args = args
        self.decks = {n: Deck(n) for n in (1, 2, 3, 4)}
        self.lock = threading.Lock()
        self.uploads = {n: [] for n in self.decks}   # packets still to send
        self.wanted = {n: "" for n in self.decks}    # path each deck should show
        self.cache = {}
        self.work = threading.Condition()
        self.stop = threading.Event()

    def listen(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.bind("\0montero/flx10")
        s.settimeout(0.5)
        while not self.stop.is_set():
            try:
                msg = json.loads(s.recv(1 << 20))
            except socket.timeout:
                continue
            except ValueError:
                continue
            t = msg.get("t", time.time() * 1000) / 1000.0
            with self.lock:
                for n, d in zip((1, 2, 3, 4), msg.get("decks", [])):
                    deck = self.decks[n]
                    was = deck.path if deck.loaded else ""
                    deck.update(d, t)
                    now = deck.path if deck.loaded else ""
                    if now != was:
                        log(f"deck {n}: {'loaded ' + now.rsplit('/', 1)[-1] if now else 'unloaded'}")
                        self.uploads[n] = []
                        deck.last_wall = None
                        deck.ready = False
                        with self.work:
                            self.wanted[n] = now
                            self.work.notify()

    def waveformer(self):
        while not self.stop.is_set():
            with self.work:
                job = next(((n, p) for n, p in self.wanted.items() if p), None)
                if not job:
                    self.work.wait(0.5)
                    continue
                n, path = job
                self.wanted[n] = ""
            with self.lock:
                duration = self.decks[n].duration
            try:
                if path not in self.cache:
                    t0 = time.monotonic()
                    local = local_path(path, self.args.media_root)
                    self.cache[path] = waveform_packets(DECK_BYTES[n], local, duration,
                                                        self.args.max_cols, self.args.squeeze)
                    log(f"deck {n}: waveform ready in {time.monotonic() - t0:.1f}s")
                pkts = [bytes([DECK_BYTES[n]]) + p[1:] for p in self.cache[path]]
            except (subprocess.CalledProcessError, OSError, TypeError) as e:
                log(f"deck {n}: no waveform for {path}: {e}")
                with self.lock:
                    self.decks[n].ready = True      # show the deck without one
                continue
            with self.lock:
                if (self.decks[n].path if self.decks[n].loaded else "") == path:
                    self.uploads[n] = pkts

    def drive(self, ctl):
        """The 10 ms loop: keepalive, a state packet per loaded deck, then a
        few waveform packets."""
        next_tick = time.monotonic()
        while not self.stop.is_set():
            now_wall = time.time()
            ctl.keepalive(time.monotonic())
            with self.lock:
                pings = [d.packet(now_wall) for d in self.decks.values()
                         if d.loaded and d.ready]
                wave = []
                for n in self.decks:
                    while self.uploads[n] and len(wave) < WAVE_PACKETS_PER_TICK:
                        wave.append(self.uploads[n].pop(0))
                        if not self.uploads[n]:
                            self.decks[n].ready = True
                            log(f"deck {n}: waveform sent")
            for p in pings + wave:
                ctl.screen(p)
            next_tick += TICK
            time.sleep(max(0.0, next_tick - time.monotonic()))

    def run(self):
        threading.Thread(target=self.listen, daemon=True).start()
        threading.Thread(target=self.waveformer, daemon=True).start()
        while not self.stop.is_set():
            sysfs = find_device()
            hid = find_hidraw(sysfs) if sysfs else None
            client = find_seq_client() if sysfs else None
            if not (hid and client):
                time.sleep(2)
                continue
            log(f"FLX10 at {sysfs} (hidraw {hid}, sequencer client {client})")
            ctl = None
            try:
                ctl = Controller(sysfs, hid, client)
                # Re-send every loaded deck's waveform to a fresh controller.
                with self.lock, self.work:
                    for n, d in self.decks.items():
                        d.ready = False
                        if d.loaded:
                            self.wanted[n] = d.path
                    self.work.notify()
                log("screens live")
                self.drive(ctl)
            except OSError as e:
                log(f"controller lost: {e}")
                time.sleep(2)
            finally:
                if ctl:
                    ctl.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--media-root", default="/srv/engine/media",
                    help="where Engine's /media is on this host")
    ap.add_argument("--max-cols", type=int, default=24500,
                    help="cap waveform columns (0 = none); the firmware drops bigger uploads")
    ap.add_argument("--squeeze", action="store_true",
                    help="fit long tracks into the cap instead of clipping their end")
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("need root")
    bridge = Bridge(args)
    signal.signal(signal.SIGTERM, lambda *_: bridge.stop.set())
    try:
        bridge.run()
    except KeyboardInterrupt:
        bridge.stop.set()


if __name__ == "__main__":
    main()
