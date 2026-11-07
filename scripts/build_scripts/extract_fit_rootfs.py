#!/usr/bin/env python3
"""Extract and verify the rootfs from an inMusic FIT firmware image.

  extract_fit_rootfs.py <firmware.img> <rootfs.img>

Prints the rootfs size in bytes on stdout; everything else goes to stderr.

Older inMusic updates are plain U-Boot FIT images, a flattened devicetree
(magic d00dfeed) whose /images/<name> nodes carry each payload inline in a
`data` property, with `compression` and a `hash` subnode beside it. Seen on
Prime GO 2.4.0 to 4.3.0, MPC 2.7.0, HeadRush Pedalboard 2.7 and Looperboard
2.0.5, all with an xz rootfs and a SHA-1. binwalk 3 does not unpack these: it
matches the devicetree header, claims the whole file as one DTB and stops, so
the rootfs inside is never reached.

Written from the devicetree specification and U-Boot's FIT format
(doc/usage/fit/source_file_format.rst), stdlib only. The rootfs is found by its
`partition = "rootfs"` property rather than its node name, and streamed: the
compressed payload is hashed while it is decompressed, so the image is read
once and never held in memory.
"""

import hashlib
import io
import lzma
import os
import shutil
import struct
import sys
import zlib

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9
COPY_SIZE = 1024 * 1024
# Property values at or under this size are read while walking the tree. Larger
# ones, which in practice are only `data`, are recorded as (offset, length).
SMALL_VALUE = 4096


class Crc32:
    """hashlib-shaped wrapper so FIT's crc32 algo verifies like the others."""

    def __init__(self):
        self.value = 0

    def update(self, data):
        self.value = zlib.crc32(data, self.value)

    def digest(self):
        return struct.pack(">I", self.value)


def new_hash(algo):
    if algo == "crc32":
        return Crc32()
    try:
        return hashlib.new(algo)
    except ValueError:
        return None


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
            raise EOFError("FIT rootfs data ends before its declared length")
        for h in self.hashes:
            h.update(data)
        buffer[: len(data)] = data
        self.remaining -= len(data)
        return len(data)


def read_cstring(image):
    value = bytearray()
    while True:
        byte = image.read(1)
        if not byte:
            raise ValueError("devicetree node name runs past the end of the image")
        if byte == b"\0":
            return value.decode("ascii", "replace")
        value.extend(byte)


def walk(image, image_size):
    """Return {path: {property: value or (offset, length)}} for every node."""
    header = image.read(40)
    if len(header) < 40:
        raise ValueError("image is too short to be a FIT")
    (magic, total_size, struct_off, strings_off, _, version, last_comp,
     _, strings_size, struct_size) = struct.unpack(">10I", header)
    if magic != FDT_MAGIC:
        raise ValueError("not a FIT image (no d00dfeed devicetree header)")
    # struct_size and strings_size arrived in version 3 and 17. Every inMusic
    # image seen is 17; anything that cannot be read as 16 is refused.
    if version < 17 or last_comp > 16:
        raise ValueError(f"unsupported devicetree version {version} (last compatible {last_comp})")
    if total_size > image_size:
        raise ValueError("devicetree totalsize runs past the end of the image")
    if struct_off + struct_size > total_size or strings_off + strings_size > total_size:
        raise ValueError("devicetree blocks run past its totalsize")

    image.seek(strings_off)
    strings = image.read(strings_size)

    nodes = {}
    path = []
    pos, end = struct_off, struct_off + struct_size
    while pos < end:
        image.seek(pos)
        (token,) = struct.unpack(">I", image.read(4))
        pos += 4
        if token == FDT_BEGIN_NODE:
            name = read_cstring(image)
            pos += (len(name.encode()) + 4) & ~3
            path.append(name)
            nodes["/".join(path) or "/"] = {}
        elif token == FDT_END_NODE:
            if not path:
                raise ValueError("devicetree closes more nodes than it opens")
            path.pop()
        elif token == FDT_PROP:
            length, name_off = struct.unpack(">II", image.read(8))
            pos += 8
            if pos + length > end or name_off >= strings_size:
                raise ValueError("devicetree property runs past its block")
            name = strings[name_off:strings.index(b"\0", name_off)].decode("ascii", "replace")
            value = image.read(length) if length <= SMALL_VALUE else (pos, length)
            nodes["/".join(path) or "/"][name] = value
            pos += (length + 3) & ~3
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            return nodes
        else:
            raise ValueError(f"unknown devicetree token {token} at 0x{pos - 4:x}")
    raise ValueError("devicetree has no end token")


def text(value):
    return value.split(b"\0", 1)[0].decode("ascii", "replace") if isinstance(value, bytes) else ""


def find_rootfs(nodes):
    images = [p for p in nodes if p.startswith("/images/") and p.count("/") == 2]
    for want in (lambda p: text(nodes[p].get("partition")) == "rootfs",
                 lambda p: p == "/images/rootfs"):
        match = [p for p in images if want(p)]
        if match:
            return match[0]
    raise ValueError("FIT image has no rootfs under /images")


def extract(source_path, dest_path):
    image_size = os.path.getsize(source_path)
    partial = dest_path + ".part"
    try:
        with open(source_path, "rb") as image:
            nodes = walk(image, image_size)
            node = find_rootfs(nodes)
            props = nodes[node]

            data = props.get("data")
            if data is None:
                # External data (data-offset, data-position) is part of the FIT
                # format but no inMusic image uses it, so it is not handled here.
                raise ValueError(f"{node} has no inline data")
            offset, length = data if isinstance(data, tuple) else (None, len(data))
            if offset is None:
                raise ValueError(f"{node} data is only {length} bytes")

            # Every hash subnode with an algorithm Python knows is checked; one
            # it does not know is reported and skipped rather than trusted.
            checks = []
            for sub in sorted(p for p in nodes if p.startswith(node + "/hash")):
                algo, expected = text(nodes[sub].get("algo")), nodes[sub].get("value")
                h = new_hash(algo)
                if h is None or not isinstance(expected, bytes):
                    print(f"--- skipping {sub}: cannot check algo '{algo}' ---", file=sys.stderr)
                    continue
                checks.append((sub, algo, h, expected))
            if not checks:
                raise ValueError(f"{node} has no hash this reader can verify")

            compression = text(props.get("compression")) or "none"
            version = text(nodes["/"].get("inmusic,version")) or "unknown"
            print(f"--- FIT {text(nodes['/'].get('compatible'))} {version}: {node} "
                  f"at 0x{offset:x}, {length // (1024 * 1024)} MiB {compression} ---",
                  file=sys.stderr)

            image.seek(offset)
            reader = io.BufferedReader(HashingReader(image, length, [c[2] for c in checks]), COPY_SIZE)
            with open(partial, "wb") as out:
                if compression == "xz":
                    with lzma.LZMAFile(reader, "rb") as source:
                        shutil.copyfileobj(source, out, COPY_SIZE)
                elif compression == "none":
                    shutil.copyfileobj(reader, out, COPY_SIZE)
                else:
                    raise ValueError(f"{node} uses unsupported compression '{compression}'")
                # LZMAFile stops at the end of the last stream it finds; trailing
                # bytes would not be hashed, so drain them into the check.
                while reader.read(COPY_SIZE):
                    pass

            for sub, algo, h, expected in checks:
                if h.digest() != expected:
                    raise ValueError(f"{node} {algo} does not match {sub}")

        with open(partial, "rb") as out:
            out.seek(1024 + 56)
            if out.read(2) != b"\x53\xef":
                raise ValueError(f"{node} is not an ext2/3/4 filesystem")
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
