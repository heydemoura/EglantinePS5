#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create an unrelocated AArch64 ELF analysis carrier from a local LZ4 NSO."""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nso", type=Path)
    parser.add_argument("out", type=Path, help="new output directory for private analysis")
    args = parser.parse_args()
    data = args.nso.read_bytes()
    if len(data) < 256 or data[:4] != b"NSO0":
        parser.error("expected an NSO0 header")
    flags = struct.unpack_from("<I", data, 12)[0]
    if flags & 0x80:
        parser.error("ZBIC compression is not supported by this analysis helper")
    headers = [struct.unpack_from("<IIII", data, 16 + i * 16) for i in range(3)]
    if max(v + n + (extra if i == 2 else 0)
           for i, (_, v, n, extra) in enumerate(headers)) > 512 << 20:
        parser.error("analysis image exceeds 512 MiB")
    lz4 = ctypes.CDLL(ctypes.util.find_library("lz4"))
    lz4.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_int, ctypes.c_int]
    segments = []
    for i, (offset, address, size, _) in enumerate(headers):
        stored = struct.unpack_from("<I", data, 96 + i * 4)[0] if flags & (1 << i) else size
        if offset + stored > len(data):
            parser.error("segment exceeds file")
        payload = data[offset:offset + stored]
        if flags & (1 << i):
            output = ctypes.create_string_buffer(size)
            if lz4.LZ4_decompress_safe(payload, output, stored, size) != size:
                parser.error("LZ4 segment size mismatch")
            payload = output.raw
        digest = hashlib.sha256(payload).digest()
        if flags & (1 << (i + 3)) and digest != data[0xa0 + i * 32:0xc0 + i * 32]:
            parser.error("NSO segment SHA256 mismatch")
        segments.append(payload)
    # Preserve module-relative addresses and the original RX/R/RW segment permissions.
    image = bytearray(0x1000)
    offsets = []
    for payload in segments:
        image.extend(b"\0" * (-len(image) % 0x1000))
        offsets.append(len(image))
        image.extend(payload)
    names = b"\0.text\0.rodata\0.data\0.bss\0.dynstr\0.dynsym\0.shstrtab\0"
    names_offset = len(image)
    image.extend(names)
    image.extend(b"\0" * (-len(image) % 8))
    section_offset = len(image)
    image[:64] = struct.pack("<16sHHIQQQIHHHHHH",
        b"\x7fELF\x02\x01\x01" + b"\0" * 9, 3, 183, 1, headers[0][1],
        64, section_offset, 0, 64, 56, 3, 64, 8, 7)
    for i, ((_, address, size, extra), offset, permissions) in enumerate(
            zip(headers, offsets, [5, 4, 6])):
        image[64 + i * 56:120 + i * 56] = struct.pack("<IIQQQQQQ", 1, permissions,
            offset, address, address, size, size + (extra if i == 2 else 0), 0x1000)
    sections = [bytes(64)]
    def section(name, kind, attrs, address, offset, size, link=0, info=0, align=1, entry=0):
        sections.append(struct.pack("<IIQQQQIIQQ", names.index(name.encode() + b"\0"),
                                    kind, attrs, address, offset, size, link, info, align, entry))
    for i, (name, attrs) in enumerate(zip([".text", ".rodata", ".data"], [6, 2, 3])):
        section(name, 1, attrs, headers[i][1], offsets[i], len(segments[i]), align=4)
    section(".bss", 8, 3, headers[2][1] + headers[2][2],
            offsets[2] + headers[2][2], headers[2][3], align=4)
    symbols = []
    extents = [struct.unpack_from("<II", data, p) for p in [0x90, 0x98]]
    for (name, kind), (offset, size) in zip([(".dynstr", 3), (".dynsym", 11)], extents):
        if offset + size > len(segments[1]):
            parser.error("dynamic section exceeds read-only segment")
        section(name, kind, 2, headers[1][1] + offset, offsets[1] + offset, size,
                link=5 if kind == 11 else 0, align=8 if kind == 11 else 1,
                entry=24 if kind == 11 else 0)
    strings_offset, strings_size = extents[0]
    strings = segments[1][strings_offset:strings_offset + strings_size]
    symbols_offset, symbols_size = extents[1]
    for offset in range(symbols_offset, symbols_offset + symbols_size, 24):
        name, info, other, index, value, size = struct.unpack_from("<IBBHQQ", segments[1], offset)
        # NSO retains the original ELF section indices. Map defined symbols to
        # the sections in this carrier instead of treating all code as rodata.
        if 0 < index < 0xff00:
            mapped = next((i + 1 for i, (_, address, count, _) in enumerate(headers)
                           if address <= value < address + count), None)
            if mapped is None and headers[2][1] + headers[2][2] <= value < \
                    headers[2][1] + headers[2][2] + headers[2][3] + 1:
                mapped = 4
            if mapped is None:
                parser.error("defined symbol lies outside the NSO segments")
            struct.pack_into("<H", image, offsets[1] + offset + 6, mapped)
        if name < len(strings):
            end = strings.find(b"\0", name)
            symbols.append(dict(name=strings[name:end].decode(errors="replace"),
                                address=value, size=size, info=info, section=index))
    section(".shstrtab", 3, 0, 0, names_offset, len(names))
    image.extend(b"".join(sections))
    args.out.mkdir(parents=True, exist_ok=False)
    target = args.out / (args.nso.name + ".elf")
    target.write_bytes(image)
    (args.out / "symbols.json").write_text(json.dumps(symbols, indent=2))
    receipt = dict(source=str(args.nso.resolve()), source_sha256=hashlib.sha256(data).hexdigest(),
                   carrier_sha256=hashlib.sha256(image).hexdigest(), segments=headers,
                   limitations=["Derived analysis carrier; not the original game ELF.",
                                "Addresses are unrelocated module offsets, not live guest VAs.",
                                "No game code is executed by this helper."])
    (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2))
    print(target)


if __name__ == "__main__":
    main()
