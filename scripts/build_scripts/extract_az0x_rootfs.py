#!/usr/bin/env python3
"""Extract and verify the rootfs payload from an inMusic AZ0x image."""

import hashlib
import io
import lzma
import os
import shutil
import struct
import sys


DESCRIPTOR_SIZE = 0x40
COPY_SIZE = 1024 * 1024


class LimitedReader(io.RawIOBase):
    def __init__(self, source, size):
        self.source = source
        self.remaining = size

    def readable(self):
        return True

    def readinto(self, buffer):
        if self.remaining == 0:
            return 0
        size = min(len(buffer), self.remaining)
        data = self.source.read(size)
        if not data:
            raise EOFError("AZ0x payload ended before its declared size")
        buffer[: len(data)] = data
        self.remaining -= len(data)
        return len(data)


def read_c_string(image, offset, limit):
    if not 0 <= offset < limit:
        raise ValueError(f"string offset 0x{offset:x} is outside the string table")
    image.seek(offset)
    value = bytearray()
    while image.tell() < limit:
        byte = image.read(1)
        if byte == b"\0":
            return value.decode("ascii")
        if not byte:
            break
        value.extend(byte)
    raise ValueError(f"unterminated string at 0x{offset:x}")


def find_rootfs(image, image_size):
    header = image.read(0x28)
    if len(header) != 0x28 or header[:4] != b"AZ0x":
        raise ValueError("not an AZ0x firmware image")

    version, _, strings_offset, _, devices_offset, parts_offset, parts_end, _ = (
        struct.unpack_from("<8I", header, 4)
    )
    device_count, partition_count = struct.unpack_from("<BxB", header, 0x24)
    if version != 1:
        raise ValueError(f"unsupported AZ0x format version {version}")
    if devices_offset != strings_offset + struct.unpack_from("<I", header, 0x20)[0]:
        raise ValueError("invalid AZ0x string table bounds")
    if parts_offset + partition_count * DESCRIPTOR_SIZE != parts_end:
        raise ValueError("invalid AZ0x partition table bounds")
    if devices_offset + device_count * 8 != parts_offset:
        raise ValueError("invalid AZ0x device table bounds")

    candidates = []
    image.seek(parts_offset)
    descriptors = image.read(partition_count * DESCRIPTOR_SIZE)
    for index in range(partition_count):
        descriptor = descriptors[index * DESCRIPTOR_SIZE : (index + 1) * DESCRIPTOR_SIZE]
        kind = descriptor[:4]
        start, size, name_offset, device_mask = struct.unpack_from("<QQII", descriptor, 8)
        digest = descriptor[0x20:0x40]
        if kind != b"PART":
            continue
        name = read_c_string(image, strings_offset + name_offset, devices_offset)
        if name == "rootfs":
            if start > image_size or size > image_size - start:
                raise ValueError("rootfs payload extends past the end of the image")
            candidates.append((device_mask != 0, -size, index, start, size, digest))

    if not candidates:
        raise ValueError("AZ0x image has no rootfs partition")
    _, _, index, start, size, digest = min(candidates)
    return index, start, size, digest


def extract(source_path, destination_path):
    image_size = os.path.getsize(source_path)
    partial_path = destination_path + ".part"
    try:
        with open(source_path, "rb") as image:
            index, start, size, expected_digest = find_rootfs(image, image_size)
            print(
                f"--- AZ0x rootfs descriptor {index}: offset=0x{start:x}, "
                f"compressed={size // (1024 * 1024)} MiB ---",
                file=sys.stderr,
            )

            image.seek(start)
            digest = hashlib.sha256()
            remaining = size
            while remaining:
                data = image.read(min(COPY_SIZE, remaining))
                if not data:
                    raise EOFError("AZ0x rootfs payload is truncated")
                digest.update(data)
                remaining -= len(data)
            if digest.digest() != expected_digest:
                raise ValueError("AZ0x rootfs SHA-256 does not match its descriptor")

            image.seek(start)
            limited = io.BufferedReader(LimitedReader(image, size), COPY_SIZE)
            with lzma.LZMAFile(limited, "rb") as compressed, open(partial_path, "wb") as output:
                shutil.copyfileobj(compressed, output, COPY_SIZE)

        with open(partial_path, "rb") as output:
            output.seek(1024 + 56)
            if output.read(2) != b"\x53\xef":
                raise ValueError("decompressed rootfs is not an ext2/3/4 filesystem")
        os.replace(partial_path, destination_path)
        return os.path.getsize(destination_path)
    except Exception:
        try:
            os.unlink(partial_path)
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
