# texture-store

Offline texture conversion for EglantinePS5. The PS5's GPU reads BC formats but not ASTC, so the emulator
converts every ASTC texture while an area loads (on the GPU, to BC1/BC3). This tool does that conversion ahead of
time on a PC, at the best quality BC7 allows; the emulator then uploads the converted file instead of decoding.

Ported from PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7, `ps5/tools/texture-store/`,
GPL-3.0-or-later). The file format, the key and the code are PS5_Lichii's; only the paths and this text were
adapted. The emulator side is described in [docs/TEXTURE-STORE.md](../../docs/TEXTURE-STORE.md).

Nothing here has been run for EglantinePS5 yet: see "Untested" in that document.

Game files, and everything made from them, stay out of this repository: the tool only reads the unpacked game and
writes to a folder you name. A store is made from your own dump and is never distributed.

## What it was written for

One title: The Legend of Zelda: Ocarina of Time for Switch 2 (`0400486024DEC000`), whose unpacked romfs holds its
textures as `.bntx.zs` (or `.bntx`) files. PS5_Lichii's notes say all 11,739 ASTC textures of that game pass the
tool's checks. For any other game, read "Another game" below before spending hours of encoding.

## Two routes

- **`store` (the one the emulator uses)**: the game keeps its ASTC files. Each ASTC texture becomes one BC7 file
  named by the hash of the bytes the game hands the GPU; the emulator looks the hash up and uploads the BC7 as it
  is.
- **`convert` (set aside by PS5_Lichii, kept as it was)**: a romfs mod that replaces the ASTC files with BC7 (BC5
  for normal maps) and raises the game's resource size table to fit. PS5_Lichii found the textures then take about
  1.8 times the memory inside the game. It is specific to that title's resource size table and was not looked at
  for EglantinePS5.

`scan` reports exact duplicates and flat-colour textures. It changes nothing.

## The store

`<store>/<key>.bc7`, key = 16 lower-case hex digits of CityHash64 (the emulator's own
`Eglantine/src/common/cityhash.cpp`, compiled into the tool) over the texture's guest bytes: its data in the BNTX
from the first byte of the base level of layer 0, block-linear as stored, `CalculateGuestSizeInBytes` long. The
tool computes that length after the emulator's code and refuses a texture whose length differs from the file's
image size.

Little endian:

| Field | |
|---|---|
| `char magic[8]` | `PS5BC7T1` |
| `u32 width, height, depth, layers, levels` | of the base level, as the ASTC source |
| `u32 guest_bytes` | the length hashed |
| `u64 guest_hash` | the key |
| `levels` x (`u64 offset`, `u64 size`) | from the start of the file; a level holds all its layers, one after another |
| data | per layer of each level: 4x4 BC7 blocks row by row, no swizzle, no padding |

- The decoded RGBA keeps ASTC's channel layout (a normal map stays X in red, Y in alpha), so the game's channel
  mapping applies unchanged. Nothing is converted between sRGB and linear.
- Channels the shader cannot see cost nothing: the channel selector of the BNTX says which are read; unread alpha
  is stored opaque and unread colour channels copy red. Where alpha is read, a fully opaque block decodes to
  exactly 255.
- Textures with the same guest bytes share one file, encoded for every channel any of them is read through.
- Each level and layer is decoded from its own ASTC data (ARM astcenc) and encoded by bc7e at "very slow" with
  plain mean-square error.
- Before a file is written, every level is decoded back with a different decoder (texture2ddecoder) and compared
  with the decoded source. The PSNR goes to the log `<store>.jsonl`; under 25 dB counts as a failure.
- Restartable: a file that exists with the right header and length is skipped; delete a file to redo it.

## What it needs

A Linux PC (x86-64 with AVX2: the encoder is built for it), and:

- Python 3.14 or newer. The tool imports `compression.zstd`, which older Pythons do not have.
- Python packages: `numpy`, `astc-encoder-py` (ARM astcenc, imported as `astc_encoder`) and `texture2ddecoder`.
- `ispc` (Intel's compiler, for bc7e) and `g++` with C++20.
- `curl`, `sha256sum` and `tar`, used once by `build.sh`.
- The game's romfs, unpacked from your own dump.
- Time and disk: every ASTC texture is encoded at bc7e's "very slow" setting on all cores (`--jobs`), and the
  store is about as large as the textures are as BC7 (one byte a texel, plus mip levels).

```sh
./build.sh    # fetches bc7enc_rdo (pinned, checksum verified) into ~/.cache/texture-store and builds libbc7tex.so
```

`build.sh` downloads from github.com. `TEXTURE_STORE_CACHE` moves the cache folder (the build and the tool must
agree on it).

## Use

```sh
texture_store.py store   --game <romfs> --out <store>                               # every ASTC texture
texture_store.py store   --game <romfs> --out <store> --first Field Player           # files with those words first
texture_store.py store   --game <romfs> --out <store> --only MaterialAlb Tex/Foo_Nrm.bntx.zs @list.txt
texture_store.py scan    --game <romfs> --report <somewhere outside a repository>/scan.json
texture_store.py convert --game <romfs> --out <mod>/romfs [--only ...] [--untouched]
```

`<store>` is a folder outside this repository. The run can be stopped and started again.

## On the console

Copy the `.bc7` files (not the `.jsonl` log) to the folder of the game's program id, 16 lower-case hex digits:

```text
/data/eglantineps5/texture-store/0400486024dec000/<16 hex digits>.bc7
```

Sub-folders are read too. The emulator reads the folder when a game starts and logs
`Texture store: N BC7 images under ...`; while the game runs, `Texture store: N ASTC images found, M not` says how
many of the textures the game used were in the store. `EDEN_NX2_TEXTURE_STORE_DIR=<folder>` names another folder
and `EDEN_NX2_TEXTURE_STORE=0` turns the store off. Without the folder the emulator does what it did before.

## Another game

The emulator side does not care which game a store is for: it only needs files whose key is the hash of the bytes
the game gives the GPU. The tool is where the title shows. Before a full run on another game:

1. **Get BNTX files.** The tool walks the romfs for `*.bntx.zs` and `*.bntx`. A game that keeps its textures in
   archives (SARC, `.pack`, `.bfres` with embedded textures, a format of its own) needs them unpacked to BNTX
   first, or `texture_files()` and `parse()` written for its container.
2. **Check the tiling.** `addresses()` untiles block-linear data in the Switch 2 order (GOB offset bits 5 and 6
   swapped against the Switch 1 order). A Switch 1 title needs the Switch 1 order there. Nothing in the tool
   catches the wrong one: the BC7 is compared with the tool's own decode of the same blocks, so a scrambled
   texture passes with a good PSNR. Decode one texture and look at it.
3. **Read the assertions as findings.** `parse()` and `check()` assert what held for the one game: tile mode 0,
   depth 1, one sample, textures back to back in the data block, the relocation table where it was. A game that
   fails one needs that relation looked at, not the assertion removed. 3D textures are refused (depth 1 is
   asserted); cube maps were not looked at.
4. **Run `store --only` on a few textures** and read the first lines: `N ASTC textures; guest size differs from
   the file's image size in M`. M must be 0: otherwise the bytes the tool hashes are not the bytes the emulator
   hashes.
5. **Prove the keys on the console** with those few files: the emulator must log them as found. A key only
   matches when the game hands the GPU the BNTX data byte for byte, with every mip level, as one image. Textures
   the game builds or repacks at run time, or describes to the GPU with fewer levels than the file, are never
   found; they are decoded as before.
6. Images whose guest data is over 64 MiB are never looked up (the emulator's scan limit), so their store files
   are unused.
7. Then the full run, copied to `/data/eglantineps5/texture-store/<program id>/`.
