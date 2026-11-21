#!/usr/bin/env python3
"""Write one product's devicetree out of a firmware update image.

  extract_dtb.py <firmware image> <product code> <output .dtb>

Exits 0 having written the blob, or 1 if the image holds none for that product.

The AZ05 generation (Prime GO+ from 5.x, SC LIVE 2 and 4, Mixstream Pro+ and
Pro GO) keeps its devicetrees in the boot partition of the AZ0x container and
not under /boot in the rootfs, where detect_audio_card.sh looks first. The
partition is stored uncompressed, so the blobs can be found by their FDT magic
without parsing the container.

Each product appears three times: two U-Boot devicetrees and the kernel's. Only
the kernel's has the sound node detect_audio_card.sh reads, so the blob taken is
the largest whose root compatible starts with inmusic,<product> and that carries
an inmusic,*-audio compatible.

Stdlib only, and run with python3 -I, as the other extractors here are.
"""
import mmap
import re
import struct
import sys

FDT_MAGIC = b"\xd0\x0d\xfe\xed"
FDT_BEGIN_NODE, FDT_PROP = 1, 3


def root_compatible(blob):
    """The root node's first compatible string, or None."""
    off_struct, off_strings = struct.unpack_from(">II", blob, 8)
    p = off_struct
    if struct.unpack_from(">I", blob, p)[0] != FDT_BEGIN_NODE:
        return None
    p = (blob.index(b"\0", p + 4) + 4) & ~3
    # The root's properties come before its first child node.
    while struct.unpack_from(">I", blob, p)[0] == FDT_PROP:
        size, name_off = struct.unpack_from(">II", blob, p + 4)
        value = blob[p + 12:p + 12 + size]
        name = blob[off_strings + name_off:blob.index(b"\0", off_strings + name_off)]
        if name == b"compatible":
            return value.split(b"\0")[0].decode("ascii", "replace")
        p = (p + 12 + size + 3) & ~3
    return None


def main():
    firmware, product, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    want = "inmusic," + product
    audio = re.compile(rb"inmusic,[a-z0-9]+-audio\0")
    best = None
    with open(firmware, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        pos = 0
        while (i := m.find(FDT_MAGIC, pos)) >= 0:
            pos = i + 4
            if i + 40 > len(m):
                break
            total, off_struct, off_strings = struct.unpack_from(">III", m, i + 4)
            # Magic bytes turn up in compressed data too; a real header has its
            # blocks inside a blob of plausible size.
            if not 64 <= total <= 1 << 20 or off_struct >= total or off_strings >= total:
                continue
            blob = bytes(m[i:i + total])
            try:
                if root_compatible(blob) != want:
                    continue
            except (struct.error, ValueError):
                continue
            if audio.search(blob) and (best is None or len(blob) > len(best)):
                best = blob
    if best is None:
        return 1
    with open(out, "wb") as f:
        f.write(best)
    return 0


if __name__ == "__main__":
    sys.exit(main())
