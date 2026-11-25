#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Store files from the emulator's own dump of the ASTC images a game used.

For a game whose texture files texture_store.py cannot read. Run the game with
EDEN_NX2_TEXTURE_STORE_DUMP=<folder on the console> in env.txt: every ASTC image the texture store
does not have is written to that session's dump-<time>.pack there, already keyed and with its
blocks in rows. Copy the folder to a PC and:

    from_dump.py --dump <folder of .pack files> --out <store folder> [--level -1] [--jobs N]

then copy the .bc7 files to /data/eglantineps5/texture-store/<program id>/ on the console. The key
is the one the emulator took, so a file made here is found by construction. The store holds only
what the dumped session showed: play further with the dump on and convert again to add to it.

The store file's format is PS5_Lichii's (README.md); the dump's is in
Eglantine/src/video_core/renderer_vulkan/vk_texture_cache.cpp (DumpAstc). Needs libbc7tex.so
(build.sh), numpy and astc-encoder-py; texture2ddecoder too for --check.
"""
import argparse
import ctypes
import json
import math
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np

DUMP_MAGIC = b"PS5ASTD1"
STORE_MAGIC = b"PS5BC7T1"
_lib = None
_astc = {}


def div_up(a, b):
    return -(-a // b)


def lib():
    global _lib
    if _lib is None:
        cache = os.environ.get("TEXTURE_STORE_CACHE", os.path.expanduser("~/.cache/texture-store"))
        _lib = ctypes.CDLL(os.path.join(cache, "libbc7tex.so"))
        _lib.tex_bc7.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5 + [ctypes.c_char_p]
        _lib.tex_init()
    return _lib


def astc_decode(blocks, w, h, bw, bh, srgb):
    import astc_encoder as ae
    key = (bw, bh, srgb)
    if key not in _astc:
        _astc[key] = ae.ASTCContext(ae.ASTCConfig(ae.ASTCProfile.LDR_SRGB if srgb else ae.ASTCProfile.LDR, bw, bh, 1, 60.0))
    image = ae.ASTCImage(ae.ASTCType.U8, w, h, 1)
    _astc[key].decompress(blocks, image, ae.ASTCSwizzle.from_str("RGBA"))
    return np.frombuffer(image.data, np.uint8).reshape(h, w, 4)


def header(head, where):
    magic, w, h, layers, levels, bw, bh, srgb, guest, key = struct.unpack("<8s8IQ", head)
    assert magic == DUMP_MAGIC, "not a dump record: " + where
    return {"w": w, "h": h, "layers": layers, "levels": levels, "bw": bw, "bh": bh, "srgb": srgb, "guest": guest,
            "key": key}


def records(folder):
    """(file, offset, header) of every whole record: a .pack holds many, a .astc (the first dumps) one."""
    out = {}
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if not name.endswith((".pack", ".astc")):
            continue
        size, at = os.path.getsize(path), 0
        with open(path, "rb") as f:
            while at + 48 <= size:
                f.seek(at)
                t = header(f.read(48), "%s at %d" % (path, at))
                n = 48 + astc_bytes(t)
                if at + n > size:   # the app was ended while this one was on its way
                    break
                out.setdefault(t["key"], (path, at, t))
                at += n
    return list(out.values())


def astc_bytes(t):
    return sum(div_up(max(1, t["w"] >> i), t["bw"]) * div_up(max(1, t["h"] >> i), t["bh"]) for i in range(t["levels"])) \
        * 16 * t["layers"]


def convert(path, offset, t, out, level, check, preview):
    """One dump record as a store file. Returns a record for the log."""
    start = time.time()
    per = [div_up(max(1, t["w"] >> i), 4) * div_up(max(1, t["h"] >> i), 4) * 16 for i in range(t["levels"])]
    head = struct.pack("<8s6IQ", STORE_MAGIC, t["w"], t["h"], 1, t["layers"], t["levels"], t["guest"], t["key"])
    at = len(head) + 16 * t["levels"]
    for n in per:
        head += struct.pack("<QQ", at, n * t["layers"])
        at += n * t["layers"]
    dst = os.path.join(out, "%016x.bc7" % t["key"])
    if os.path.exists(dst) and os.path.getsize(dst) == at:
        return {"key": "%016x" % t["key"], "skipped": True, "bytes": at}
    err, count, opaque = 0.0, 0, True
    with open(path, "rb") as f, open(dst + ".part", "wb") as g:
        f.seek(offset + 48)
        g.write(head)
        for i in range(t["levels"]):
            w, h = max(1, t["w"] >> i), max(1, t["h"] >> i)
            n = div_up(w, t["bw"]) * div_up(h, t["bh"]) * 16
            for layer in range(t["layers"]):
                px = np.ascontiguousarray(astc_decode(f.read(n), w, h, t["bw"], t["bh"], t["srgb"]))
                if i == 0 and opaque:
                    opaque = bool((px[..., 3] == 255).all())
                blocks = ctypes.create_string_buffer(per[i])
                # Every channel may be read (the dump does not say which), and an opaque texel stays opaque.
                lib().tex_bc7(px.ctypes.data, w, h, level, 1, 1, blocks)
                g.write(blocks.raw)
                if i == 0 and layer == 0:
                    if check:
                        import texture2ddecoder as t2d
                        back = np.frombuffer(t2d.decode_bc7(blocks.raw, w, h), np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]
                        err += float(((back.astype(np.int32) - px) ** 2).sum())
                        count += px.size
                    if preview:
                        from PIL import Image
                        Image.fromarray(px, "RGBA").save(os.path.join(preview, "%016x.png" % t["key"]))
    os.replace(dst + ".part", dst)
    rec = {"key": "%016x" % t["key"], "w": t["w"], "h": t["h"], "layers": t["layers"], "levels": t["levels"],
           "block": "%dx%d" % (t["bw"], t["bh"]), "srgb": t["srgb"], "opaque": opaque, "guest": t["guest"], "bytes": at,
           "seconds": round(time.time() - start, 2)}
    if count:
        rec["psnr0"] = round(99.0 if err == 0 else 10 * math.log10(255 * 255 * count / err), 1)
    return rec


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dump", required=True, help="the dump folder copied from the console")
    p.add_argument("--out", required=True, help="the store folder (outside any repository)")
    p.add_argument("--level", type=int, default=-1,
                   help="BC7 effort: -3 ultra fast, -2 very fast, -1 fast (default), 0 basic, 1 slow, 2 very slow, 3 slowest")
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("--check", action="store_true", help="decode level 0 of each file with another decoder and log its PSNR")
    p.add_argument("--preview", help="a folder for a PNG of level 0, layer 0 of the --preview-count largest images")
    p.add_argument("--preview-count", type=int, default=12)
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
    files = sorted(records(a.dump), key=lambda r: astc_bytes(r[2]), reverse=True)   # the largest first
    shown = set(r[2]["key"] for r in files[:a.preview_count]) if a.preview else set()
    log = a.out.rstrip(os.sep) + ".jsonl"
    start = time.time()
    done = made = size = failed = 0
    with ProcessPoolExecutor(a.jobs) as pool, open(log, "a") as f:
        jobs = {pool.submit(convert, path, at, t, a.out, a.level, a.check, a.preview if t["key"] in shown else None):
                "%016x" % t["key"] for path, at, t in files}
        for job in as_completed(jobs):
            done += 1
            try:
                rec = job.result()
            except Exception as e:   # one bad dump must not cost the rest
                failed += 1
                print("FAILED %s: %s" % (jobs[job], e), file=sys.stderr)
                continue
            size += rec["bytes"]
            if not rec.get("skipped"):
                made += 1
                f.write(json.dumps(rec) + "\n")
            if done % 500 == 0 or done == len(files):
                print("%d of %d (%d made, %d failed), %.2f GiB, %.0f s" % (done, len(files), made, failed, size / 2 ** 30,
                                                                             time.time() - start), flush=True)


if __name__ == "__main__":
    main()
