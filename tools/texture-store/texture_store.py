#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# From PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7), ps5/tools/texture-store/texture_store.py.
# Changed here: this header, the texts that name the emulator and its paths. The store's file format, its key and
# the code that makes them are PS5_Lichii's.
"""Offline texture tool for EglantinePS5, written for The Legend of Zelda: Ocarina of Time (Switch 2,
0400486024DEC000). See README.md.

    texture_store.py store   --game <romfs> --out <store folder> [--only NAME ...] [--first WORD ...] [--level 0-3]
    texture_store.py convert --game <romfs> --out <mod romfs> [--only NAME ...] [--untouched] [--level 0-3] [--jobs N]
    texture_store.py scan    --game <romfs> --report <file.json> [--jobs N]

Reads the game's folder, never writes there. What it writes is made from the game: keep it out of any repository.
"""
import argparse
import ctypes
import functools
import glob
import hashlib
import json
import math
import os
import struct
import sys
import time
import zlib
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np
from compression import zstd

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
ASTC = {0x2D: (4, 4), 0x2E: (5, 4), 0x2F: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6), 0x34: (8, 8),
        0x35: (10, 5), 0x36: (10, 6), 0x37: (10, 8), 0x38: (10, 10), 0x39: (12, 10), 0x3A: (12, 12)}
BC = {0x1A: ("BC1", 8), 0x1B: ("BC2", 16), 0x1C: ("BC3", 16), 0x1D: ("BC4", 8), 0x1E: ("BC5", 16), 0x1F: ("BC6H", 16),
      0x20: ("BC7", 16)}
RAW = {0x02: 1, 0x09: 2, 0x0A: 2, 0x0B: 4, 0x0C: 4, 0x0F: 4, 0x15: 8}   # bytes per texel
NORMAL_SEL = 0x01000502    # shader reads red, alpha, 0, 1: a two-channel normal map stored as ASTC "rrrg"
BC5_SEL = 0x01000302       # red, green, 0, 1: how the game's own BC5 normal maps are read
MIN_PSNR = 25.0            # below this an output counts as a tool bug and is not written (noisy normal maps in BC7
                           # reach down to 29 dB; a wrong layout gives about 10)


def div_up(a, b):
    return (a + b - 1) // b


def fmt_name(fmt):
    t = fmt >> 8
    name = "ASTC%dx%d" % ASTC[t] if t in ASTC else BC[t][0] if t in BC else "0x%02x" % t
    return name + {1: "", 6: "_SRGB"}.get(fmt & 0xFF, "_%d" % (fmt & 0xFF))


def block_of(t):
    """(texels across a block, texels down, bytes) for a format type"""
    if t in ASTC:
        return ASTC[t] + (16,)
    if t in BC:
        return 4, 4, BC[t][1]
    return 1, 1, RAW[t]


def standard_layout(rows):
    """log2 of the block height the packer picks for a level of `rows` block rows (16 gobs, halved while it fits)"""
    bh = 16
    while bh > 1 and rows <= 8 * bh // 2:
        bh //= 2
    return bh.bit_length() - 1


def levels_of(w, h, mips, layers, t, block_height):
    """Block-linear placement of one array layer: per level (offset in the layer, blocks across, blocks down, block
    height, bytes), and the layer's size. Checked against all 21,655 one-texture files of the game."""
    bw, bh_, bpb = block_of(t)
    out, size = [], 0
    for level in range(mips):
        wb, hb = div_up(max(1, w >> level), bw), div_up(max(1, h >> level), bh_)
        bh = block_height
        while bh > 1 and hb <= 8 * bh // 2:
            bh //= 2
        n = div_up(wb * bpb, 64) * 64 * div_up(hb, 8 * bh) * 8 * bh
        out.append((size, wb, hb, bh, n))
        size += n
    if layers > 1:
        align = 512 * out[0][3]
        size = div_up(size, align) * align
    return out, size


@functools.lru_cache(maxsize=32)
def addresses(wb, hb, bpb, block_height):
    """Index (in elements of bpb bytes) of every block (row, column) in the block-linear layout: 64-byte x 8-row gobs,
    block_height gobs to a block."""
    y, x = np.mgrid[0:hb, 0:wb]
    xb = x * bpb
    xi, yi = xb % 64, y % 8
    gobs_x = div_up(wb * bpb, 64)
    row = y // (8 * block_height) * 512 * block_height * gobs_x + (y % (8 * block_height)) // 8 * 512
    at = (row + xb // 64 * 512 * block_height + (xi >> 5 & 1) * 256 + (yi >> 2 & 1) * 128 + (xi >> 4 & 1) * 64
          + (yi >> 1 & 1) * 32 + (yi & 1) * 16 + xi % 16)
    return (at // bpb).astype(np.int64)


def untile(data, wb, hb, bpb, block_height):
    """a level's blocks in row order, as bytes"""
    return np.frombuffer(data, np.dtype("V%d" % bpb))[addresses(wb, hb, bpb, block_height)].tobytes()


def tile(blocks, wb, hb, bpb, block_height, size):
    out = np.zeros(size // bpb, np.dtype("V%d" % bpb))
    out[addresses(wb, hb, bpb, block_height)] = np.frombuffer(blocks, np.dtype("V%d" % bpb)).reshape(hb, wb)
    return out.tobytes()


# ---------------------------------------------------------------------------------------------------- BNTX

def parse(raw):
    """(header facts, textures) of a BNTX file. Offsets are absolute."""
    assert raw[:4] == b"BNTX", "not a BNTX file"
    rlt, size = struct.unpack_from("<II", raw, 0x18)
    count, = struct.unpack_from("<I", raw, 0x24)
    infos, brtd = struct.unpack_from("<QQ", raw, 0x28)
    texs = []
    for at in struct.unpack_from("<%dQ" % count, raw, infos):
        assert raw[at:at + 4] == b"BRTI"
        flags, dim, tile_mode, swizzle, mips, samples, fmt, access, w, h, depth, layers, layout = \
            struct.unpack_from("<BBHHHIIIiiiiI", raw, at + 0x10)
        image, align, sel, kind = struct.unpack_from("<IIII", raw, at + 0x50)
        name_at, = struct.unpack_from("<Q", raw, at + 0x60)
        ptrs, = struct.unpack_from("<Q", raw, at + 0x70)
        assert tile_mode == 0 and depth == 1 and samples == 1 and layout < 5, "unexpected texture kind"
        texs.append({"at": at, "mips": mips, "fmt": fmt, "w": w, "h": h, "layers": max(1, layers), "layout": layout,
                     "image": image, "sel": sel, "ptrs": ptrs, "offs": struct.unpack_from("<%dQ" % mips, raw, ptrs),
                     "name": raw[name_at + 2:name_at + 2 + struct.unpack_from("<H", raw, name_at)[0]].decode()})
    return {"rlt": rlt, "size": size, "brtd": brtd}, texs


def check(raw):
    """Every size and offset of the file agrees with its textures; returns parse(raw). The converter relies on exactly
    these relations, so they are asserted on the source and again on the output."""
    hdr, texs = parse(raw)
    rlt, brtd = hdr["rlt"], hdr["brtd"]
    assert hdr["size"] == len(raw) and raw[brtd:brtd + 4] == b"BRTD" and raw[rlt:rlt + 4] == b"_RLT"
    end = brtd + 16
    slots = {0x30}
    for t in texs:
        levels, layer = levels_of(t["w"], t["h"], t["mips"], t["layers"], t["fmt"] >> 8, 1 << t["layout"])
        assert t["offs"][0] == end, "textures are not back to back"
        assert [o - end for o in t["offs"]] == [lv[0] for lv in levels], "level offsets"
        assert t["image"] == layer * t["layers"], "image size"
        end += t["image"]
        slots.update(t["ptrs"] + 8 * i for i in range(t["mips"]))
    assert rlt == div_up(end, 0x1000) * 0x1000, "relocation table place"
    assert struct.unpack_from("<Q", raw, brtd + 8)[0] == rlt - brtd, "data block size"
    own, sections = struct.unpack_from("<II", raw, rlt + 4)
    assert own == rlt and sections == 2
    pos, size, first, n = struct.unpack_from("<IIII", raw, rlt + 16 + 24 + 8)
    assert pos == brtd and size == rlt - brtd, "relocation section of the data"
    found = set()   # the pointers the table says lead into the data: the data block's and the level tables, no others
    for k in range(first, first + n):
        at, structs, offsets, padding = struct.unpack_from("<IHBB", raw, rlt + 16 + 48 + 8 * k)
        for _ in range(structs):
            found.update(at + 8 * i for i in range(offsets))
            at += 8 * (offsets + padding)
    assert found == slots, "pointers into the data"
    return hdr, texs


def rebuild(raw, hdr, texs, new):
    """The file with texture i replaced by new[i] = (format, layout, channel selector, data) where given."""
    out = bytearray(raw[:hdr["brtd"] + 16])
    end = len(out)
    data = []
    for t, n in zip(texs, new):
        fmt, layout, sel, blob = n or (t["fmt"], t["layout"], t["sel"], raw[t["offs"][0]:t["offs"][0] + t["image"]])
        levels, layer = levels_of(t["w"], t["h"], t["mips"], t["layers"], fmt >> 8, 1 << layout)
        assert len(blob) == layer * t["layers"]
        struct.pack_into("<I", out, t["at"] + 0x1C, fmt)
        struct.pack_into("<I", out, t["at"] + 0x34, layout)
        struct.pack_into("<I", out, t["at"] + 0x50, len(blob))
        struct.pack_into("<I", out, t["at"] + 0x58, sel)
        struct.pack_into("<%dQ" % t["mips"], out, t["ptrs"], *(end + lv[0] for lv in levels))
        data.append(blob)
        end += len(blob)
    rlt = div_up(end, 0x1000) * 0x1000
    table = bytearray(raw[hdr["rlt"]:])
    struct.pack_into("<I", table, 4, rlt)
    struct.pack_into("<I", table, 16 + 24 + 12, rlt - hdr["brtd"])
    struct.pack_into("<II", out, 0x18, rlt, rlt + len(table))
    struct.pack_into("<Q", out, hdr["brtd"] + 8, rlt - hdr["brtd"])
    return bytes(out) + b"".join(data) + bytes(rlt - end) + bytes(table)


# ---------------------------------------------------------------------------------------------------- codecs

_lib = None
_astc = {}


def lib():
    global _lib
    if _lib is None:
        cache = os.environ.get("TEXTURE_STORE_CACHE", os.path.expanduser("~/.cache/texture-store"))
        _lib = ctypes.CDLL(os.path.join(cache, "libbc7tex.so"))   # built by build.sh
        _lib.tex_bc7.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5 + [ctypes.c_char_p]
        _lib.tex_cityhash64.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _lib.tex_cityhash64.restype = ctypes.c_uint64
        _lib.tex_bc5.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5 + [ctypes.c_char_p]
        _lib.tex_init()
    return _lib


def astc_decode(blocks, w, h, bw, bh, srgb):
    """ARM astcenc: a level's ASTC blocks to (h, w, 4) RGBA bytes"""
    import astc_encoder as ae
    key = (bw, bh, srgb)
    if key not in _astc:
        _astc[key] = ae.ASTCContext(ae.ASTCConfig(ae.ASTCProfile.LDR_SRGB if srgb else ae.ASTCProfile.LDR, bw, bh, 1, 60.0))
    image = ae.ASTCImage(ae.ASTCType.U8, w, h, 1)
    _astc[key].decompress(blocks, image, ae.ASTCSwizzle.from_str("RGBA"))
    return np.frombuffer(image.data, np.uint8).reshape(h, w, 4)


def bc_decode(t, blocks, w, h):
    """texture2ddecoder (independent of the encoders): a level's BC blocks to (h, w, 4) RGBA bytes, None for BC2"""
    import texture2ddecoder as t2d
    f = {0x1A: t2d.decode_bc1, 0x1C: t2d.decode_bc3, 0x1D: t2d.decode_bc4, 0x1E: t2d.decode_bc5, 0x1F: t2d.decode_bc6,
         0x20: t2d.decode_bc7}.get(t)
    if f is None:
        return None
    return np.frombuffer(f(blocks, w, h), np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]


def used_channels(sel):
    """which stored channels (0-3) the shader can see through the channel selector"""
    return sorted({(sel >> s & 0xFF) - 2 for s in (0, 8, 16, 24)} - {-2, -1})


def transcode(raw, t, level, used, bc5):
    """Every layer and level of an ASTC texture encoded as BC7 (or as BC5 from red and alpha), each from its own ASTC
    data. Yields (layer, level, blocks in row order, squared error against the decoded source, values compared);
    the error is measured by decoding the blocks with a decoder that is not the encoder's, on the channels `used`."""
    bw, bh, _ = block_of(t["fmt"] >> 8)
    assert t["fmt"] & 0xFF in (1, 6), "ASTC variant"
    src, src_layer = levels_of(t["w"], t["h"], t["mips"], t["layers"], t["fmt"] >> 8, 1 << t["layout"])
    for layer in range(t["layers"]):
        for i in range(t["mips"]):
            w, h = max(1, t["w"] >> i), max(1, t["h"] >> i)
            at, wb, hb, block_height, n = src[i]
            at += t["offs"][0] + layer * src_layer
            px = astc_decode(untile(raw[at:at + n], wb, hb, 16, block_height), w, h, bw, bh, t["fmt"] & 0xFF == 6).copy()
            # channels the shader never reads must not cost the encoder anything
            if 3 not in used:
                px[..., 3] = 255
            if used in ([0], [0, 3]):
                px[..., 1] = px[..., 2] = px[..., 0]
            blocks = ctypes.create_string_buffer(div_up(w, 4) * div_up(h, 4) * 16)
            if bc5:
                lib().tex_bc5(px.ctypes.data, w, h, 0, 3, 8, blocks)
            else:   # (red copied to green and blue counts three times: alpha gets the same weight)
                lib().tex_bc7(px.ctypes.data, w, h, level, 3 in used, 3 if used == [0, 3] else 1, blocks)
            got = bc_decode(0x1E if bc5 else 0x20, blocks.raw, w, h)
            a, b = (px[..., [0, 3]], got[..., :2]) if bc5 else (px[..., used], got[..., used])
            yield layer, i, blocks.raw, float(((a.astype(np.int32) - b) ** 2).sum()), a.size


def convert_texture(raw, t, level):
    """One ASTC texture as BC7, or BC5 for a normal map, laid out for the game: (format, layout, selector, data), and
    error sums [squared error of level 0, values of level 0, squared error of all levels, values of all]."""
    normal = t["sel"] == NORMAL_SEL
    fmt, sel = (0x1E01, BC5_SEL) if normal else (0x2000 | t["fmt"] & 0xFF, t["sel"])
    # the game's files are packed either with block height 1 throughout or by the standard rule: keep the file's way
    layout = 0 if t["layout"] == 0 else standard_layout(div_up(t["h"], 4))
    dst, dst_layer = levels_of(t["w"], t["h"], t["mips"], t["layers"], fmt >> 8, 1 << layout)
    out = bytearray(dst_layer * t["layers"])
    err = [0.0, 0, 0.0, 0]
    for layer, i, blocks, e, count in transcode(raw, t, level, used_channels(t["sel"]), normal):
        at, wb, hb, block_height, n = dst[i]
        at += layer * dst_layer
        out[at:at + n] = tile(blocks, wb, hb, 16, block_height, n)
        assert untile(out[at:at + n], wb, hb, 16, block_height) == blocks
        if i == 0:
            err[0] += e
            err[1] += count
        err[2] += e
        err[3] += count
    return (fmt, layout, sel, bytes(out)), err


def psnr(e, n):
    return 99.0 if e == 0 else round(10 * math.log10(255.0 ** 2 * n / e), 2)


def pack(data):
    """Strongest standard zstd: level 22, window no larger than the game's own files use (8 MiB), content size written,
    no checksum, no dictionary. A plain frame any zstd decoder reads."""
    P = zstd.CompressionParameter
    return zstd.compress(data, options={P.compression_level: 22, P.window_log: 23, P.checksum_flag: 0})


def unpack(packed):
    return zstd.decompress(packed) if packed[:4] == ZSTD_MAGIC else packed


def raw_size(path):
    with open(path, "rb") as f:
        head = f.read(32)
    return zstd.get_frame_info(head).decompressed_size if head[:4] == ZSTD_MAGIC else os.path.getsize(path)


# ---------------------------------------------------------------------------------------------------- convert

def convert_file(game, out, rel, level, untouched):
    """Convert one file. Returns a record for the log, or None when the file needs no output."""
    start = time.time()
    src, dst = os.path.join(game, rel), os.path.join(out, rel)
    packed = open(src, "rb").read()
    raw = unpack(packed)
    hdr, texs = check(raw)
    todo = [t["fmt"] >> 8 in ASTC for t in texs]
    if not any(todo) and not untouched:
        return None
    rec = {"rel": rel, "src_disk": len(packed), "src_raw": len(raw), "src_image": sum(t["image"] for t in texs),
           "textures": []}
    if os.path.exists(dst):   # an earlier run's output: keep it if it is whole and what this run would make
        done = open(dst, "rb").read()
        _, got = check(unpack(done))
        assert len(got) == len(texs) and not any(g["fmt"] >> 8 in ASTC for g in got), "stale output: delete " + dst
        assert all((g["w"], g["h"], g["mips"], g["layers"]) == (t["w"], t["h"], t["mips"], t["layers"])
                   for g, t in zip(got, texs)), "stale output: delete " + dst
        return {"rel": rel, "skipped": True}
    new, err = [], [0.0, 0, 0.0, 0]
    for t, astc in zip(texs, todo):
        if not astc:
            new.append(None)
            continue
        n, e = convert_texture(raw, t, level)
        new.append(n)
        err = [a + b for a, b in zip(err, e)]
        rec["textures"].append({"name": t["name"], "from": fmt_name(t["fmt"]), "to": fmt_name(n[0]),
                                "w": t["w"], "h": t["h"], "mips": t["mips"], "layers": t["layers"],
                                "psnr0": psnr(e[0], e[1]), "psnr": psnr(e[2], e[3])})
    if any(todo):
        result = rebuild(raw, hdr, texs, new)
        _, got = check(result)
        rec.update(psnr0=psnr(err[0], err[1]), psnr=psnr(err[2], err[3]))
        assert rec["psnr0"] >= MIN_PSNR, "level 0 is only %.1f dB from the source" % rec["psnr0"]
        rec["dst_image"] = sum(g["image"] for g in got)
    else:
        result = raw
        rec["dst_image"] = rec["src_image"]
    stored = pack(result) if packed[:4] == ZSTD_MAGIC else result
    assert unpack(stored) == result
    rec.update(dst_raw=len(result), dst_disk=len(stored), seconds=round(time.time() - start, 1))
    if not any(todo) and len(stored) >= len(packed):
        rec["kept_original"] = True   # recompression gained nothing: the mod does not need this file
        return rec
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".part", "wb") as f:
        f.write(stored)
    os.replace(dst + ".part", dst)
    return rec


def size_table(game, out):
    """The game sets aside memory per file from its resource size table and drops a file larger than its entry, so the
    mod carries the table with the entry of every file in the mod raised to fit (same overhead as the entry had)."""
    src, = glob.glob(os.path.join(game, "System", "Resource", "ResourceSizeTable.*.rsizetable.zs"))
    table = bytearray(unpack(open(src, "rb").read()))
    assert table[:6] == b"RESTBL"
    name_len, hashed, named = struct.unpack_from("<III", table, 10)
    hashes = np.frombuffer(bytes(table), "<u4", hashed * 2, 22)[::2]
    names = {bytes(table[22 + 8 * hashed + i * (name_len + 4):][:name_len]).rstrip(b"\0").decode():
             22 + 8 * hashed + i * (name_len + 4) + name_len for i in range(named)}
    raised = 0
    for folder, _, files in os.walk(out):
        for f in files:
            if not f.endswith(".bntx.zs"):
                continue
            rel = os.path.relpath(os.path.join(folder, f), out)
            key = rel[:-3].replace(os.sep, "/")
            sizes = [raw_size(os.path.join(root, rel)) for root in (game, out)]
            if key in names:
                at = names[key]
            else:
                k = int(np.searchsorted(hashes, zlib.crc32(key.encode())))
                if k >= hashed or hashes[k] != zlib.crc32(key.encode()):
                    continue   # the game has no entry for this file (three such textures) and loads it regardless
                at = 22 + 8 * k + 4
            old, = struct.unpack_from("<I", table, at)
            over = old - div_up(sizes[0], 32) * 32
            need = div_up(sizes[1], 32) * 32 + (over if over >= 0 else 264)
            if need > old:
                struct.pack_into("<I", table, at, need)
                raised += 1
    dst = os.path.join(out, os.path.relpath(src, game))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".part", "wb") as f:
        f.write(pack(bytes(table)))
    os.replace(dst + ".part", dst)
    return raised


def texture_files(game, only=None):
    """the game's texture files (paths in the data folder), largest first; `only`: paths, or texture names"""
    found = []
    for folder, _, files in os.walk(game):
        for f in files:
            if f.endswith(".bntx.zs") or f.endswith(".bntx"):
                p = os.path.join(folder, f)
                found.append((os.path.getsize(p), os.path.relpath(p, game)))
    if only:
        want = set()
        for o in only:
            want.update(line.strip() for line in open(o[1:])) if o.startswith("@") else want.add(o)
        found = [x for x in found if x[1] in want or os.path.basename(x[1]).split(".bntx")[0] in want]
        missing = want - {x[1] for x in found} - {os.path.basename(x[1]).split(".bntx")[0] for x in found}
        assert not missing, "not in the game: %s" % sorted(missing)
    return [rel for _, rel in sorted(found, reverse=True)]


def run(pool, jobs, show):
    """results of the jobs as they finish; a failed file is reported and the rest go on"""
    futures = {pool.submit(*job): job for job in jobs}
    failed = 0
    for n, f in enumerate(as_completed(futures), 1):
        try:
            yield f.result()
        except Exception as e:   # noqa: BLE001 (one bad file must not stop a run of hours)
            failed += 1
            print("FAILED %s: %r" % (show(futures[f]), e), flush=True)
        if n % 200 == 0 or n == len(futures):
            print("%d / %d files, %d failed" % (n, len(futures), failed), flush=True)


def convert(a):
    game, out = os.path.realpath(a.game), os.path.realpath(a.out)
    assert not (out + os.sep).startswith(game + os.sep) and out != game, "refused: the output is inside the game's data"
    log = out.rstrip(os.sep) + ".convert.jsonl"   # beside the mod's romfs folder
    os.makedirs(out, exist_ok=True)
    start = time.time()
    # files an earlier run found not worth recompressing have no output to skip by: skip them by the log
    kept = {r["rel"] for r in map(json.loads, open(log))} if os.path.exists(log) else set()
    kept = {r for r in kept if not os.path.exists(os.path.join(out, r))}
    with ProcessPoolExecutor(a.jobs) as pool, open(log, "a") as f:
        jobs = [(convert_file, game, out, rel, a.level, a.untouched) for rel in texture_files(game, a.only)
                if rel not in kept]
        for rec in run(pool, jobs, lambda job: job[3]):
            if rec and not rec.get("skipped"):
                f.write(json.dumps(rec) + "\n")
                f.flush()
    print("size table: %d entries raised" % size_table(game, out))
    summary(log, time.time() - start)


def summary(log, seconds=None):
    recs = {}
    for line in open(log):
        r = json.loads(line)
        recs[r["rel"]] = r   # a file converted again replaces its earlier record
    conv = [r for r in recs.values() if r["textures"]]
    rest = [r for r in recs.values() if not r["textures"]]
    by = defaultdict(int)
    for r in conv:
        for t in r["textures"]:
            by[t["to"]] += 1
    gib = lambda n: "%.2f GiB" % (n / 2 ** 30)   # noqa: E731
    print("converted files: %d (textures by output format: %s)" % (len(conv), dict(by)))
    if conv:
        print("  on disk   %s -> %s" % (gib(sum(r["src_disk"] for r in conv)), gib(sum(r["dst_disk"] for r in conv))))
        print("  in memory %s -> %s" % (gib(sum(r["src_image"] for r in conv)), gib(sum(r["dst_image"] for r in conv))))
        for key, what in (("psnr0", "level 0"), ("psnr", "all levels")):
            v = sorted(r[key] for r in conv)
            print("  PSNR %s, dB per file: min %.1f, 1%% %.1f, median %.1f, mean %.1f"
                  % (what, v[0], v[len(v) // 100], v[len(v) // 2], sum(v) / len(v)))
        print("  lowest: %s" % ", ".join("%s %.1f" % (os.path.basename(r["rel"]), r["psnr0"])
                                         for r in sorted(conv, key=lambda r: r["psnr0"])[:5]))
    if rest:
        kept = sum(1 for r in rest if r.get("kept_original"))
        print("recompressed only: %d files, on disk %s -> %s (%d gained nothing and are not in the mod)"
              % (len(rest), gib(sum(r["src_disk"] for r in rest)),
                 gib(sum(min(r["src_disk"], r["dst_disk"]) for r in rest)), kept))
    if seconds is not None:
        print("this run: %.0f s" % seconds)


# ---------------------------------------------------------------------------------------------------- store

STORE_MAGIC = b"PS5BC7T1"


def guest_size(t):
    """CalculateGuestSizeInBytes of the emulator (Eglantine/src/video_core/texture_cache/util.cpp, the same code as
    PS5_Lichii's) for this texture as the game describes it to the GPU: written after that code, not after levels_of,
    so the two can be compared."""
    bw, bh, bpb = block_of(t["fmt"] >> 8)
    size = 0
    for level in range(t["mips"]):
        rows = div_up(max(1, t["h"] >> level), bh)
        shift = t["layout"]                                       # TileShift / AdjustTileSize
        if not (level == 0 and t["mips"] == 1) and shift:
            x = 8 << (shift - 1)
            if x >= rows:
                while True:
                    shift -= 1
                    if shift == 0:
                        break
                    x >>= 1
                    if x < rows:
                        break
        gobs_x = div_up(div_up(max(1, t["w"] >> level), bw) * bpb, 64)   # NumGobs (tile width spacing 0)
        size += gobs_x * div_up(div_up(rows, 8), 1 << shift) << (9 + shift)   # CalculateLevelSize
    if t["layers"] == 1:
        return size                                               # CalculateLayerSize
    block = t["layout"]                                           # AlignLayerSize
    while block and div_up(t["h"], bh) * bh <= (1 << (block - 1)) * 8:
        block -= 1
    return div_up(size, 512 << block) * (512 << block) * t["layers"]


def store_index(game, rel):
    """the ASTC textures of one file: (key, file, index in the file, channels read, bytes in the store, size facts)"""
    raw = unpack(open(os.path.join(game, rel), "rb").read())
    _, texs = check(raw)
    out = []
    for k, t in enumerate(texs):
        if t["fmt"] >> 8 in ASTC:
            n = guest_size(t)
            key = lib().tex_cityhash64(raw[t["offs"][0]:t["offs"][0] + n], n)
            bc = sum(div_up(max(1, t["w"] >> i), 4) * div_up(max(1, t["h"] >> i), 4) for i in range(t["mips"]))
            out.append((key, rel, k, used_channels(t["sel"]), bc * 16 * t["layers"], n, t["image"]))
    return out


def store_texture(game, store, rel, index, key, used, level):
    """One ASTC texture as a store file <key>.bc7 (the layout is in the README). Returns a record for the log."""
    start = time.time()
    raw = unpack(open(os.path.join(game, rel), "rb").read())
    t = check(raw)[1][index]
    guest = guest_size(t)
    assert guest == t["image"], "guest size %d is not the file's image size %d" % (guest, t["image"])
    assert lib().tex_cityhash64(raw[t["offs"][0]:t["offs"][0] + guest], guest) == key
    per = [div_up(max(1, t["w"] >> i), 4) * div_up(max(1, t["h"] >> i), 4) * 16 for i in range(t["mips"])]
    head = struct.pack("<8s6IQ", STORE_MAGIC, t["w"], t["h"], 1, t["layers"], t["mips"], guest, key)
    at = len(head) + 16 * t["mips"]
    for n in per:
        head += struct.pack("<QQ", at, n * t["layers"])
        at += n * t["layers"]
    dst = os.path.join(store, "%016x.bc7" % key)
    if os.path.exists(dst):   # an earlier run's file: keep it if it is whole
        with open(dst, "rb") as f:
            assert os.path.getsize(dst) == at and f.read(len(head)) == head, "stale store file: delete " + dst
        return {"key": "%016x" % key, "skipped": True}
    data = [bytearray(n * t["layers"]) for n in per]
    err = [0.0, 0, 0.0, 0]
    for layer, i, blocks, e, count in transcode(raw, t, level, used, False):
        data[i][layer * per[i]:(layer + 1) * per[i]] = blocks
        if i == 0:
            err[0] += e
            err[1] += count
        err[2] += e
        err[3] += count
    rec = {"key": "%016x" % key, "rel": rel, "name": t["name"], "from": fmt_name(t["fmt"]), "w": t["w"], "h": t["h"],
           "mips": t["mips"], "layers": t["layers"], "used": used, "guest": guest, "bytes": at,
           "psnr0": psnr(err[0], err[1]), "psnr": psnr(err[2], err[3]), "seconds": round(time.time() - start, 1)}
    assert rec["psnr0"] >= MIN_PSNR, "level 0 is only %.1f dB from the source" % rec["psnr0"]
    with open(dst + ".part", "wb") as f:
        f.write(head + b"".join(data))
    os.replace(dst + ".part", dst)
    return rec


def store(a):
    game, out = os.path.realpath(a.game), os.path.realpath(a.out)
    assert not (out + os.sep).startswith(game + os.sep) and out != game, "refused: the output is inside the game's data"
    os.makedirs(out, exist_ok=True)
    log = out.rstrip(os.sep) + ".jsonl"   # beside the store folder, so the folder holds store files only
    start = time.time()
    with ProcessPoolExecutor(a.jobs) as pool:
        found = []
        for r in run(pool, [(store_index, game, rel) for rel in texture_files(game, a.only)], lambda job: job[2]):
            found += r
        differ = [(rel, k, n, image) for _, rel, k, _, _, n, image in found if n != image]
        print("%d ASTC textures; guest size differs from the file's image size in %d" % (len(found), len(differ)))
        for d in differ[:20]:
            print("  %s texture %d: guest %d, file %d" % d)
        # textures with the same bytes share one file; it must serve every channel any of them is read through
        keys = {}
        for key, rel, k, used, size, _, _ in found:
            first = keys.setdefault(key, [rel, k, set(), size])
            first[2].update(used)
        print("%d store files to make (%d textures repeat another's bytes)" % (len(keys), len(found) - len(keys)))
        late = lambda rel: not any(word in rel for word in a.first or [])   # noqa: E731 (then largest first)
        jobs = [(store_texture, game, out, rel, k, key, sorted(used), a.level)
                for key, (rel, k, used, _) in sorted(keys.items(), key=lambda kv: (late(kv[1][0]), -kv[1][3]))]
        with open(log, "a") as f:
            for rec in run(pool, jobs, lambda job: "%s texture %d" % (job[3], job[4])):
                if not rec.get("skipped"):
                    f.write(json.dumps(rec) + "\n")
                    f.flush()
    recs = {}
    for line in open(log):
        r = json.loads(line)
        recs[r["key"]] = r
    recs = [r for r in recs.values() if os.path.exists(os.path.join(out, r["key"] + ".bc7"))]
    print("store: %d files, %.2f GiB; the same textures are %.2f GiB as the game holds them"
          % (len(recs), sum(r["bytes"] for r in recs) / 2 ** 30, sum(r["guest"] for r in recs) / 2 ** 30))
    for what, pick in (("red and alpha read (normal maps, grey with alpha)", lambda r: r["used"] == [0, 3]),
                       ("the rest", lambda r: r["used"] != [0, 3])):
        v = sorted(r["psnr0"] for r in recs if pick(r))
        if v:
            print("  PSNR level 0, %s: %d files, min %.1f, 1%% %.1f, median %.1f, mean %.1f dB"
                  % (what, len(v), v[0], v[len(v) // 100], v[len(v) // 2], sum(v) / len(v)))
    print("  lowest: %s" % ", ".join("%s %.1f" % (r["name"], r["psnr0"]) for r in sorted(recs, key=lambda r: r["psnr0"])[:5]))
    print("this run: %.0f s" % (time.time() - start))


# ---------------------------------------------------------------------------------------------------- scan

def flat_colour(raw, t):
    """The one colour (as the shader sees its channels) every texel of level 0 has in every layer, or None. Formats
    with no decoder here (BC2) count as not flat."""
    ft = t["fmt"] >> 8
    bw, bh, bpb = block_of(ft)
    levels, layer_size = levels_of(t["w"], t["h"], t["mips"], t["layers"], ft, 1 << t["layout"])
    _, wb, hb, block_height, n = levels[0]
    colour = None
    for layer in range(t["layers"]):
        at = t["offs"][0] + layer * layer_size
        blocks = untile(raw[at:at + n], wb, hb, bpb, block_height)
        if ft in RAW:
            px = np.frombuffer(blocks, np.uint8).reshape(-1, bpb)
        else:
            # a cheap exact test first: the top row of blocks; nearly every texture already fails there
            for rows in (1, hb):
                w, h = t["w"], min(t["h"], rows * bh)
                part = blocks[:rows * wb * bpb]
                px = astc_decode(part, w, h, bw, bh, t["fmt"] & 0xFF == 6) if ft in ASTC else bc_decode(ft, part, w, h)
                if px is None:
                    return None
                px = px[..., used_channels(t["sel"])].reshape(-1, len(used_channels(t["sel"])))
                if (px != px[0]).any():
                    return None
        if (px != px[0]).any() or (colour is not None and tuple(px[0]) != colour):
            return None
        colour = tuple(int(v) for v in px[0])
    return colour


def scan_file(game, rel):
    packed = open(os.path.join(game, rel), "rb").read()
    raw = unpack(packed)
    _, texs = check(raw)
    out = []
    for t in texs:
        data = raw[t["offs"][0]:t["offs"][0] + t["image"]]
        key = struct.pack("<6I", t["fmt"], t["w"], t["h"], t["mips"], t["layers"], t["sel"])
        out.append({"rel": rel, "name": t["name"], "fmt": fmt_name(t["fmt"]), "w": t["w"], "h": t["h"],
                    "mips": t["mips"], "layers": t["layers"], "image": t["image"],
                    "disk": len(packed) if len(texs) == 1 else None,
                    "sha1": hashlib.sha1(key + data).hexdigest(), "flat": flat_colour(raw, t)})
    return out


def scan(a):
    game = os.path.realpath(a.game)
    texs = []
    with ProcessPoolExecutor(a.jobs) as pool:
        for r in run(pool, [(scan_file, game, rel) for rel in texture_files(game)], lambda job: job[2]):
            texs += r
    groups = defaultdict(list)
    for t in texs:
        groups[t["sha1"]].append(t)
    dups = sorted((g for g in groups.values() if len(g) > 1), key=lambda g: -g[0]["image"] * (len(g) - 1))
    flat = sorted((t for t in texs if t["flat"] is not None), key=lambda t: -t["image"])
    mib = lambda n: "%.1f MiB" % (n / 2 ** 20)   # noqa: E731
    report = {
        "textures": len(texs),
        "duplicates": {"groups": len(dups), "spare_copies": sum(len(g) - 1 for g in dups),
                       "spare_bytes_in_memory": sum(g[0]["image"] * (len(g) - 1) for g in dups),
                       "spare_bytes_on_disk": sum(sum(t["disk"] or 0 for t in g[1:]) for g in dups)},
        "flat": {"textures": len(flat), "bytes_in_memory": sum(t["image"] for t in flat),
                 "bytes_on_disk": sum(t["disk"] or 0 for t in flat),
                 "larger_than_one_block": sum(1 for t in flat if t["w"] > 4 or t["h"] > 4)},
        "duplicate_groups": [[{k: t[k] for k in ("rel", "name", "fmt", "w", "h", "layers", "image", "disk")} for t in g]
                             for g in dups],
        "flat_textures": [{k: t[k] for k in ("rel", "name", "fmt", "w", "h", "mips", "layers", "image", "disk", "flat")}
                          for t in flat],
    }
    with open(a.report, "w") as f:
        json.dump(report, f, indent=1)
    d, fl = report["duplicates"], report["flat"]
    print("%d textures scanned" % len(texs))
    print("exact duplicates: %d groups, %d spare copies, %s in memory, %s on disk"
          % (d["groups"], d["spare_copies"], mib(d["spare_bytes_in_memory"]), mib(d["spare_bytes_on_disk"])))
    print("flat colour: %d textures (%d larger than 4x4), %s in memory, %s on disk"
          % (fl["textures"], fl["larger_than_one_block"], mib(fl["bytes_in_memory"]), mib(fl["bytes_on_disk"])))
    print("details: %s" % a.report)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert", help="ASTC textures to BC7/BC5 into a romfs mod; restartable")
    c.add_argument("--game", required=True, help="the game's unpacked romfs (read only)")
    c.add_argument("--out", required=True, help="the mod's romfs folder")
    c.add_argument("--only", nargs="+", help="texture names, paths in the romfs, or @file with one per line")
    c.add_argument("--untouched", action="store_true", help="also recompress the files that hold no ASTC texture")
    c.add_argument("--level", type=int, default=2, help="BC7 effort: 0 basic, 1 slow, 2 very slow (default; measured best), 3 slowest")
    c.add_argument("--jobs", type=int, default=os.cpu_count())
    c.set_defaults(f=convert)
    t = sub.add_parser("store", help="ASTC textures as BC7 files the emulator uploads by content hash; restartable")
    t.add_argument("--game", required=True, help="the game's unpacked romfs (read only)")
    t.add_argument("--out", required=True, help="the store folder")
    t.add_argument("--only", nargs="+", help="texture names, paths in the romfs, or @file with one per line")
    t.add_argument("--first", nargs="+", help="files whose path holds one of these words are made first")
    t.add_argument("--level", type=int, default=2, help="BC7 effort, as for convert")
    t.add_argument("--jobs", type=int, default=os.cpu_count())
    t.set_defaults(f=store)
    s = sub.add_parser("scan", help="report exact duplicates and flat-colour textures; changes nothing")
    s.add_argument("--game", required=True)
    s.add_argument("--report", required=True, help="where to write the JSON report (outside any repository)")
    s.add_argument("--jobs", type=int, default=os.cpu_count())
    s.set_defaults(f=scan)
    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
