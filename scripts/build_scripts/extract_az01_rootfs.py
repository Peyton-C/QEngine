#!/usr/bin/env python3
"""Extract and verify the rootfs from an inMusic AZ01 firmware image.

  extract_az01_rootfs.py <firmware.img> <rootfs.img>

Prints the rootfs size in bytes on stdout; everything else goes to stderr.

AZ01 is the container on Prime 4, Prime GO and SC6000 from 5.0 and MPC Gen1
from 3.x. Read from the images themselves, all little-endian, every string a
u32 length then NUL-terminated and padded to 4 bytes, every byte blob a u32
length then padded to 4:

  header   "AZ01", u32 version (1), u32 header length, string build tag,
           u32 count + strings compatible, u32 count + u32s device IDs,
           string description
  records  from the header length, each starting on an 8-byte boundary:
           "PART", u32 record header length, u64 data size, string name,
           string compression, u32 count + (string algo, blob digest),
           then the data
  end      an "EOF\\0" record with a 16-byte header and no data

Every image seen has one `rootfs` record, xz with a SHA-1 over the compressed
data. binwalk 3 extracts these too; this reader is here so extraction needs
no binwalk at all. The payload is hashed while it is decompressed, so the
image is read once and never held in memory.
"""

import hashlib
import io
import lzma
import os
import shutil
import struct
import sys

COPY_SIZE = 1024 * 1024
MAX_FIELD = 4096


class HashingReader(io.RawIOBase):
    """Reads `size` bytes from `source`, feeding each one to `hashes`."""

    def __init__(self, source, size, hashes):
        self.source = source
        self.remaining = size
        self.hashes = hashes

    def readable(self):
        return True

    def readinto(self, buffer):
        if self.remaining == 0:
            return 0
        data = self.source.read(min(len(buffer), self.remaining))
        if not data:
            raise EOFError("AZ01 rootfs data ends before its declared size")
        for h in self.hashes:
            h.update(data)
        buffer[: len(data)] = data
        self.remaining -= len(data)
        return len(data)


class Fields:
    """Sequential reader for AZ01's length-prefixed, 4-byte padded fields."""

    def __init__(self, image):
        self.image = image

    def u32(self):
        data = self.image.read(4)
        if len(data) != 4:
            raise ValueError("AZ01 field runs past the end of the image")
        return struct.unpack("<I", data)[0]

    def u64(self):
        return self.u32() | self.u32() << 32

    def blob(self, padded):
        length = self.u32()
        if length > MAX_FIELD:
            raise ValueError(f"AZ01 field of {length} bytes at 0x{self.image.tell() - 4:x}")
        data = self.image.read(padded(length))
        if len(data) != padded(length):
            raise ValueError("AZ01 field runs past the end of the image")
        return data[:length]

    def bytes(self):
        return self.blob(lambda n: (n + 3) & ~3)

    def string(self):
        return self.blob(lambda n: (n + 4) & ~3).decode("ascii", "replace")

    def strings(self):
        return [self.string() for _ in range(self.u32())]


def read_records(image, image_size):
    """Return the header fields and every PART record, checking the layout."""
    f = Fields(image)
    if image.read(4) != b"AZ01":
        raise ValueError("not an AZ01 firmware image")
    version, header_len = f.u32(), f.u32()
    if version != 1:
        raise ValueError(f"unsupported AZ01 format version {version}")
    tag, compatible = f.string(), f.strings()
    for _ in range(f.u32()):
        f.u32()
    f.string()
    if image.tell() != header_len:
        raise ValueError(f"AZ01 header ends at 0x{image.tell():x}, not 0x{header_len:x}")

    records = []
    pos = header_len
    while True:
        if pos + 16 > image_size:
            raise ValueError("AZ01 image ends without an EOF record")
        image.seek(pos)
        kind, record_len, data_size = image.read(4), f.u32(), f.u64()
        if kind == b"EOF\0":
            return tag, compatible, records
        if kind != b"PART":
            raise ValueError(f"unknown AZ01 record {kind!r} at 0x{pos:x}")
        name, compression = f.string(), f.string()
        digests = [(f.string(), f.bytes()) for _ in range(f.u32())]
        if image.tell() != pos + record_len:
            raise ValueError(f"AZ01 record at 0x{pos:x} is not {record_len} bytes long")
        start = pos + record_len
        if start + data_size > image_size:
            raise ValueError(f"AZ01 {name} data runs past the end of the image")
        records.append((name, compression, digests, start, data_size))
        pos = (start + data_size + 7) & ~7


def extract(source_path, dest_path):
    image_size = os.path.getsize(source_path)
    partial = dest_path + ".part"
    try:
        with open(source_path, "rb") as image:
            tag, compatible, records = read_records(image, image_size)
            rootfs = [r for r in records if r[0] == "rootfs"]
            if len(rootfs) != 1:
                raise ValueError(f"AZ01 image has {len(rootfs)} rootfs records, expected one")
            name, compression, digests, start, size = rootfs[0]

            # An algorithm hashlib does not know is reported and skipped rather
            # than trusted, but at least one digest has to check out.
            checks = []
            for algo, expected in digests:
                try:
                    checks.append((algo, hashlib.new(algo), expected))
                except ValueError:
                    print(f"--- skipping rootfs {algo}: not available here ---", file=sys.stderr)
            if not checks:
                raise ValueError("AZ01 rootfs has no digest this reader can verify")

            print(f"--- AZ01 {','.join(compatible)} {tag}: rootfs at 0x{start:x}, "
                  f"{size // (1024 * 1024)} MiB {compression} ---", file=sys.stderr)

            image.seek(start)
            reader = io.BufferedReader(HashingReader(image, size, [c[1] for c in checks]), COPY_SIZE)
            with open(partial, "wb") as out:
                if compression == "xz":
                    with lzma.LZMAFile(reader, "rb") as source:
                        shutil.copyfileobj(source, out, COPY_SIZE)
                elif compression == "none":
                    shutil.copyfileobj(reader, out, COPY_SIZE)
                else:
                    raise ValueError(f"AZ01 rootfs uses unsupported compression '{compression}'")
                # LZMAFile stops at the end of the last stream it finds; trailing
                # bytes would not be hashed, so drain them into the check.
                while reader.read(COPY_SIZE):
                    pass

            for algo, h, expected in checks:
                if h.digest() != expected:
                    raise ValueError(f"AZ01 rootfs {algo} does not match its record")

        with open(partial, "rb") as out:
            out.seek(1024 + 56)
            if out.read(2) != b"\x53\xef":
                raise ValueError("AZ01 rootfs is not an ext2/3/4 filesystem")
        os.replace(partial, dest_path)
        return os.path.getsize(dest_path)
    except BaseException:
        try:
            os.unlink(partial)
        except FileNotFoundError:
            pass
        raise


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <firmware.img> <rootfs.img>", file=sys.stderr)
        return 2
    try:
        size = extract(sys.argv[1], sys.argv[2])
    except (EOFError, OSError, ValueError, lzma.LZMAError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
