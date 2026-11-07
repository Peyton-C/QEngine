#!/usr/bin/env python3
"""Extract and verify the update image inside an inMusic desktop updater.

  extract_updater.py <updater> <update.img>

Prints the image size in bytes on stdout; everything else goes to stderr.

inMusic's firmware updater apps carry the same update image the website offers
as a bare .img, byte for byte, so an updater is as good a source as the image
itself and for some builds the only one archived. HeadRush ships nothing else.
Two packagings are read, identified by their first bytes:

  Mac zip       "PK\\3\\4", a zipped Firmware Updater.app with the image at
                Contents/Resources/Update.img, deflated. Seen from Mixstream Pro
                2.0.1 (2021) to Prime 4 G2 5.3.0 and every HeadRush updater.
  Windows .exe  "MZ", a stock 7-Zip SFX: the 7zS.sfx stub, a ;!@Install@! block
                that runs FirmwareUpdater.exe, then a 7z archive holding
                update.img, Config.json, the updater and libusb. Seen from
                Mixstream Pro 2.0.1 to Prime 4 G2 5.3.0, same stub throughout.

A bare 7z archive ("7z\\xbc\\xaf\\x27\\x1c") is read as well.

The archive is found after the stub rather than at a fixed offset, because the
stub's icon resource changes its size: 0x4e200 on Prime 4 G2 5.3.0, 0x67c00 on
Mixstream Pro 2.0.1. libarchive's bsdtar only searches a short window for the
signature and so opens the first but not the second.

The image sits in a solid LZMA2 folder of its own, with BCJ2 used only for the
folder holding the .exe and .dll, so Python's lzma decodes it with no 7-Zip
installed. The 7z reader is written from 7-Zip's DOC/7zFormat.txt and handles
only what a decoder for that needs: one-coder folders of LZMA2, LZMA or copy.
Anything else is refused by name. Zip's CRC-32 is checked by zipfile, 7z's by
this reader, and the image's own digests by the reader extract_rootfs.sh runs
next.
"""

import lzma
import os
import shutil
import struct
import sys
import zipfile
import zlib

COPY_SIZE = 1024 * 1024
SIGNATURE_7Z = b"7z\xbc\xaf\x27\x1c"
# The 7z archive follows the stub's ;!@Install@! block, a few hundred bytes. A
# window this size covers that with room to spare without reading the image.
SFX_SEARCH = 1024 * 1024
IMAGE_NAME = "update.img"

(K_END, K_HEADER, K_ARCHIVE_PROPERTIES, K_ADDITIONAL_STREAMS, K_MAIN_STREAMS,
 K_FILES_INFO, K_PACK_INFO, K_UNPACK_INFO, K_SUBSTREAMS_INFO, K_SIZE, K_CRC,
 K_FOLDER, K_CODERS_UNPACK_SIZE, K_NUM_UNPACK_STREAM, K_EMPTY_STREAM,
 K_EMPTY_FILE, K_ANTI, K_NAME) = range(18)
K_ENCODED_HEADER = 0x17

CODER_COPY, CODER_LZMA, CODER_LZMA2 = b"\x00", b"\x03\x01\x01", b"\x21"
CODER_NAMES = {b"\x03\x03\x01\x1b": "BCJ2", b"\x03\x03\x01\x03": "BCJ",
               b"\x06\xf1\x07\x01": "AES", b"\x04\x01\x08": "Deflate",
               b"\x04\x02\x02": "BZip2", b"\x03": "Delta"}


class Buffer:
    """Sequential reader for 7z's header encoding."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError("7z header runs past its end")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def byte(self):
        return self.take(1)[0]

    def number(self):
        # The count of leading 1 bits in the first byte is the count of bytes
        # that follow, little-endian; the rest of the first byte is the top.
        first = self.byte()
        value = 0
        for i in range(8):
            mask = 0x80 >> i
            if not first & mask:
                return value | (first & (mask - 1)) << (8 * i)
            value |= self.byte() << (8 * i)
        return value

    def bits(self, count):
        out, byte, mask = [], 0, 0
        for _ in range(count):
            if not mask:
                byte, mask = self.byte(), 0x80
            out.append(bool(byte & mask))
            mask >>= 1
        return out

    def defined(self, count):
        return [True] * count if self.byte() else self.bits(count)

    def digests(self, count):
        return [struct.unpack("<I", self.take(4))[0] if d else None
                for d in self.defined(count)]


class Folder:
    def __init__(self, coders, packed):
        self.coders = coders  # [(codec id, properties, out streams)]
        self.packed = packed
        self.unpack_sizes = []
        self.crc = None
        self.substreams = 1

    def size(self):
        # One-coder folders are the only ones decoded, where this is the
        # coder's sole output; for the rest it is only used to skip past them.
        return self.unpack_sizes[-1]


class Streams:
    def __init__(self):
        self.pack_pos = 0
        self.pack_sizes = []
        self.folders = []
        self.sizes = []  # per substream, in folder order
        self.crcs = []


def read_streams(buf):
    s = Streams()
    while True:
        kind = buf.number()
        if kind == K_END:
            break
        if kind == K_PACK_INFO:
            s.pack_pos = buf.number()
            count = buf.number()
            while (sub := buf.number()) != K_END:
                if sub == K_SIZE:
                    s.pack_sizes = [buf.number() for _ in range(count)]
                elif sub == K_CRC:
                    buf.digests(count)
                else:
                    raise ValueError(f"unexpected 7z pack info property {sub}")
        elif kind == K_UNPACK_INFO:
            if buf.number() != K_FOLDER:
                raise ValueError("7z unpack info without folders")
            count = buf.number()
            if buf.byte():
                raise ValueError("7z external folder data is not supported")
            s.folders = [read_folder(buf) for _ in range(count)]
            while (sub := buf.number()) != K_END:
                if sub == K_CODERS_UNPACK_SIZE:
                    for f in s.folders:
                        f.unpack_sizes = [buf.number() for _ in range(sum(c[2] for c in f.coders))]
                elif sub == K_CRC:
                    for f, crc in zip(s.folders, buf.digests(len(s.folders))):
                        f.crc = crc
                else:
                    raise ValueError(f"unexpected 7z unpack info property {sub}")
        elif kind == K_SUBSTREAMS_INFO:
            read_substreams(buf, s)
        else:
            raise ValueError(f"unexpected 7z streams property {kind}")
    if not s.sizes:
        s.sizes = [f.size() for f in s.folders]
        s.crcs = [f.crc for f in s.folders]
    return s


def read_folder(buf):
    coders, ins, outs = [], 0, 0
    for _ in range(buf.number()):
        flags = buf.byte()
        if flags & 0x80:
            raise ValueError("7z alternative coder methods are not supported")
        codec = buf.take(flags & 0x0F)
        n_in, n_out = (buf.number(), buf.number()) if flags & 0x10 else (1, 1)
        props = buf.take(buf.number()) if flags & 0x20 else b""
        coders.append((codec, props, n_out))
        ins, outs = ins + n_in, outs + n_out
    for _ in range(outs - 1):
        buf.number(), buf.number()
    packed = ins - (outs - 1)
    if packed > 1:
        for _ in range(packed):
            buf.number()
    return Folder(coders, packed)


def read_substreams(buf, s):
    kind = buf.number()
    if kind == K_NUM_UNPACK_STREAM:
        for f in s.folders:
            f.substreams = buf.number()
        kind = buf.number()
    sizes = []
    for f in s.folders:
        if f.substreams == 0:
            continue
        part = [buf.number() for _ in range(f.substreams - 1)] if kind == K_SIZE else []
        sizes += part + [f.size() - sum(part)]
    if kind == K_SIZE:
        kind = buf.number()
    # Digests are listed only for substreams the folder's own CRC does not cover.
    crcs = []
    for f in s.folders:
        if f.substreams == 1 and f.crc is not None:
            crcs.append(f.crc)
        else:
            crcs += [None] * f.substreams
    while kind != K_END:
        if kind != K_CRC:
            raise ValueError(f"unexpected 7z substreams property {kind}")
        missing = [i for i, c in enumerate(crcs) if c is None]
        for i, crc in zip(missing, buf.digests(len(missing))):
            crcs[i] = crc
        kind = buf.number()
    s.sizes, s.crcs = sizes, crcs


def read_files(buf):
    count = buf.number()
    names, empty = [""] * count, [False] * count
    while (kind := buf.number()) != K_END:
        data = Buffer(buf.take(buf.number()))
        if kind == K_EMPTY_STREAM:
            empty = data.bits(count)
        elif kind == K_NAME:
            if data.byte():
                raise ValueError("7z external file names are not supported")
            names = data.data[1:].decode("utf-16-le").split("\0")[:count]
    return names, empty


def decoder(folder, name):
    if len(folder.coders) != 1:
        chain = "+".join(CODER_NAMES.get(c[0], c[0].hex()) for c in folder.coders)
        raise ValueError(f"7z {name} is packed with {chain}, which this reader does not decode")
    codec, props, _ = folder.coders[0]
    if codec == CODER_COPY:
        return None
    if codec == CODER_LZMA2:
        p = props[0]
        dict_size = 0xFFFFFFFF if p == 40 else (2 | (p & 1)) << (p // 2 + 11)
        filters = [{"id": lzma.FILTER_LZMA2, "dict_size": dict_size}]
    elif codec == CODER_LZMA:
        d = props[0]
        filters = [{"id": lzma.FILTER_LZMA1, "lc": d % 9, "lp": d // 9 % 5, "pb": d // 45,
                    "dict_size": struct.unpack("<I", props[1:5])[0]}]
    else:
        raise ValueError(f"7z {name} uses coder {CODER_NAMES.get(codec, codec.hex())}, "
                         "which this reader does not decode")
    return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=filters)


def unpack(archive, base, streams, index, skip, size, out, name):
    """Write `size` bytes of folder `index`, after skipping `skip`, to `out`."""
    folder = streams.folders[index]
    first = sum(f.packed for f in streams.folders[:index])
    archive.seek(base + streams.pack_pos + sum(streams.pack_sizes[:first]))
    remaining_in = streams.pack_sizes[first]
    dec = decoder(folder, name)
    crc = 0
    while size:
        if dec is None or dec.needs_input:
            chunk = archive.read(min(COPY_SIZE, remaining_in))
            remaining_in -= len(chunk)
            if not chunk and (dec is None or dec.needs_input):
                raise ValueError(f"7z {name} ends before its declared size")
        else:
            chunk = b""
        data = chunk if dec is None else dec.decompress(chunk, COPY_SIZE)
        if skip:
            cut = min(skip, len(data))
            data, skip = data[cut:], skip - cut
        data = data[:size]
        crc = zlib.crc32(data, crc)
        out(data)
        size -= len(data)
    return crc


def read_7z(archive, base, dest):
    archive.seek(base)
    start = archive.read(32)
    if len(start) != 32 or start[:6] != SIGNATURE_7Z:
        raise ValueError("not a 7z archive")
    if struct.unpack_from("<I", start, 8)[0] != zlib.crc32(start[12:32]):
        raise ValueError("7z start header CRC does not match")
    offset, size, crc = struct.unpack_from("<QQI", start, 12)
    archive.seek(base + 32 + offset)
    header = archive.read(size)
    if len(header) != size or zlib.crc32(header) != crc:
        raise ValueError("7z header is truncated or its CRC does not match")
    base += 32

    buf = Buffer(header)
    while (kind := buf.number()) == K_ENCODED_HEADER:
        packed = read_streams(buf)
        chunks = []
        got = unpack(archive, base, packed, 0, 0, packed.sizes[0], chunks.append, "header")
        if packed.crcs[0] is not None and got != packed.crcs[0]:
            raise ValueError("7z encoded header CRC does not match")
        buf = Buffer(b"".join(chunks))
    if kind != K_HEADER:
        raise ValueError(f"unexpected 7z header type {kind}")

    streams, names, empty = Streams(), [], []
    while (kind := buf.number()) != K_END:
        if kind == K_MAIN_STREAMS:
            streams = read_streams(buf)
        elif kind == K_FILES_INFO:
            names, empty = read_files(buf)
        else:
            raise ValueError(f"unexpected 7z header property {kind}")

    # Files with data map in order onto substreams, folder by folder.
    stream_files = [n for n, e in zip(names, empty) if not e]
    located, cursor = [], 0
    for index, folder in enumerate(streams.folders):
        offset = 0
        for _ in range(folder.substreams):
            located.append((index, offset, streams.sizes[cursor], streams.crcs[cursor]))
            offset += streams.sizes[cursor]
            cursor += 1
    matches = [i for i, n in enumerate(stream_files) if os.path.basename(n.replace("\\", "/")).lower() == IMAGE_NAME]
    if len(matches) != 1:
        raise ValueError(f"7z archive holds {len(matches)} {IMAGE_NAME} files, expected one: "
                         + ", ".join(stream_files))
    name = stream_files[matches[0]]
    index, skip, size, crc = located[matches[0]]
    print(f"--- 7z {name}: {size // (1024 * 1024)} MiB in folder {index} ---", file=sys.stderr)
    with open(dest, "wb") as out:
        got = unpack(archive, base, streams, index, skip, size, out.write, name)
    if crc is not None and got != crc:
        raise ValueError(f"7z {name} CRC does not match")
    if crc is None:
        print(f"--- 7z {name} carries no CRC, so it is unverified here ---", file=sys.stderr)


def find_7z_after_stub(archive):
    """Return the offset of the 7z archive appended to a PE, after its sections."""
    head = archive.read(4096)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    if head[pe:pe + 4] != b"PE\0\0":
        raise ValueError("MZ file without a PE header")
    count, opt_size = struct.unpack_from("<H", head, pe + 6)[0], struct.unpack_from("<H", head, pe + 20)[0]
    table = pe + 24 + opt_size
    end = max(sum(struct.unpack_from("<II", head, table + i * 40 + 16)) for i in range(count))
    archive.seek(end)
    window = archive.read(SFX_SEARCH)
    at = window.find(SIGNATURE_7Z)
    while at >= 0:
        start = window[at:at + 32]
        if len(start) == 32 and struct.unpack_from("<I", start, 8)[0] == zlib.crc32(start[12:32]):
            return end + at
        at = window.find(SIGNATURE_7Z, at + 1)
    raise ValueError("Windows executable is not a 7-Zip SFX updater: no 7z archive after the stub")


def read_zip(path, dest):
    with zipfile.ZipFile(path) as z:
        matches = [i for i in z.infolist()
                   if not i.filename.startswith("__MACOSX/")
                   and i.filename.lower().endswith("/contents/resources/" + IMAGE_NAME)]
        if len(matches) != 1:
            raise ValueError(f"zip holds {len(matches)} .app/Contents/Resources/Update.img, expected one")
        info = matches[0]
        print(f"--- zip {info.filename}: {info.file_size // (1024 * 1024)} MiB ---", file=sys.stderr)
        # zipfile checks the member's CRC-32 when it reaches the end.
        with z.open(info) as source, open(dest, "wb") as out:
            shutil.copyfileobj(source, out, COPY_SIZE)


def extract(source_path, dest_path):
    partial = dest_path + ".part"
    try:
        with open(source_path, "rb") as archive:
            magic = archive.read(6)
            archive.seek(0)
            if magic[:4] == b"PK\3\4":
                read_zip(source_path, partial)
            elif magic[:2] == b"MZ":
                read_7z(archive, find_7z_after_stub(archive), partial)
            elif magic == SIGNATURE_7Z:
                read_7z(archive, 0, partial)
            else:
                raise ValueError("not an inMusic updater (zip, 7-Zip SFX or 7z)")
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
        print(f"Usage: {sys.argv[0]} <updater> <update.img>", file=sys.stderr)
        return 2
    try:
        size = extract(sys.argv[1], sys.argv[2])
    except (EOFError, OSError, ValueError, lzma.LZMAError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
