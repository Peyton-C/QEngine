#!/usr/bin/env python3
"""
Step bytes of the xx 27 deck state through values, to find what they drive.
Decks 1 and 2 get a fake loaded track; each byte is set to each value in turn
for a moment while the rest stay as the bridge sends them. The BPM readout
shows the step, so a change on the screen can be told apart without a clock:
23.04 is byte 23 at its fifth value.

Stop the bridge first (systemctl stop flx10-bridge); Engine can keep running.

  sudo python3 flx10_probe.py [--bytes 18,23,24,26,28] [--values bits] [--hold 1.5]
"""
import argparse, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flx10_bridge as B


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", default="18,23,24,26,28")
    ap.add_argument("--values", default="bits",
                    help="'bits' for 1,2,4..128, or a comma list")
    ap.add_argument("--hold", type=float, default=1.5)
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("need root")

    sysfs = B.find_device()
    hid = B.find_hidraw(sysfs) if sysfs else None
    client = B.find_seq_client()
    if not (hid and client):
        sys.exit("FLX10 not found")
    ctl = B.Controller(sysfs, hid, client)

    values = [1 << i for i in range(8)] if args.values == "bits" else \
             [int(v, 0) for v in args.values.split(",")]
    steps = [(b, i, v) for b in (int(x) for x in args.bytes.split(","))
             for i, v in enumerate(values)]
    steps.append((None, None, None))

    t0 = time.monotonic()
    for b, i, v in steps:
        print(f"{time.strftime('%H:%M:%S')} " +
              (f"byte {b} = 0x{v:02x} (BPM {b}.{i:02d})" if b is not None else "baseline"),
              flush=True)
        end = time.monotonic() + args.hold
        while time.monotonic() < end:
            ctl.keepalive(time.monotonic())
            t = time.monotonic() - t0
            for deck in (1, 2):
                bpm = b + i / 100 if b is not None else 128.0
                p = bytearray(B.build_xx27(B.DECK_BYTES[deck], bpm, t, 300.0, 0.0, 0x82, False))
                if b is not None:
                    p[b] = v
                ctl.screen(bytes(p))
            time.sleep(0.01)
    ctl.close()


if __name__ == "__main__":
    main()
