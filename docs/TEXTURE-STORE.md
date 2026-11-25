# The BC7 texture store

The PS5's GPU has no ASTC, so the emulator decodes every ASTC texture on the GPU and re-compresses it to BC1/BC3
in the frame that first uses it. The texture store replaces that work for textures converted ahead of time on a
PC: a folder of `<key>.bc7` files, one per texture, where the key is a hash of the bytes the game gives the GPU.
When the emulator makes an image for ASTC data it hashes the data; if the store has that key, the host image is
BC7 and is filled from the file.

Ported from PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7, GPL-3.0-or-later). Our core and
Lichii's share an ancestor but differ, so this is a port by reading, not a copy.

**Tried on the console with Mario Kart World on 2026-11-24, and it lost:** the store works (keys match, the BC7
images are right) and the game runs slower with it than without. See
[Mario Kart World on the console](#mario-kart-world-on-the-console). It stays off for every game that has no store
folder, which is every game unless you make one.

Without a store folder for the running game the emulator does what it did before: the only new work is one
attempt to open the folder when the game starts.

## Making a store

`tools/texture-store/` holds the offline tool (Python, with a BC7 encoder it builds). Its README says what it
needs, which game it was written for, and what another game takes.

### From a dump

Lichii's tool reads a game's unpacked texture files, and one game's. For any other game the emulator can write
the store's input itself:

1. `EDEN_NX2_TEXTURE_STORE_DUMP=/data/eglantineps5/texture-dump` in `env.txt`, and play. Every ASTC image the store
   does not have is written once, by its key, to that session's `dump-<time>.pack` in the folder: its ASTC blocks
   as they are, in rows (`TextureCacheRuntime::DumpAstc`). `EDEN_NX2_TEXTURE_STORE_DUMP_MIB` is the most a session
   writes (8192). It goes out 4 MiB at a time: what is not written yet when the app is ended is lost.
2. Copy the folder to a PC and run `tools/texture-store/from_dump.py --dump <folder> --out <store>`: astcenc
   decodes each level and layer, bc7e encodes it, and the file is named by the key the emulator took.
3. Copy the `.bc7` files to the game's store folder (below) and take the dump out of `env.txt`.

A key made this way is the lookup's own, so nothing has to be known about the game's files. The store holds what
the sessions dumped showed and no more: play on with the dump and convert again to add to it. A session with the
dump on is slower while new textures arrive.

With a store folder or a dump, images of up to 256 MiB of guest data are hashed (64 MiB otherwise).

## Where the store lives

```text
/data/eglantineps5/texture-store/<program id, 16 lower-case hex digits>/<key>.bc7
```

for example `/data/eglantineps5/texture-store/0400486024dec000/1f3a09c2d47be650.bc7`. Sub-folders are read too.
The program id is the running title's (`Settings::GetCurrentProgramID()`, set before the renderer is made).
`EDEN_NX2_TEXTURE_STORE_DIR=<folder>` names the folder itself instead, whatever the title.

PS5_Lichii reads one folder (`<data>/texture-store`, recursively) for every title, so a game there also sees the
other games' files. Here each game reads its own folder.

## The file

`<key>.bc7`: the key is 16 lower-case hex digits of CityHash64 (`Eglantine/src/common/cityhash.cpp`) over the
texture's guest bytes: block-linear ASTC as the game stores it, from the base level of layer 0,
`CalculateGuestSizeInBytes` long. Little endian:

| Field | |
|---|---|
| `char magic[8]` | `PS5BC7T1` |
| `u32 width, height, depth, layers, levels` | of the base level, as the ASTC source |
| `u32 guest_bytes` | the length hashed |
| `u64 guest_hash` | the key |
| `levels` x (`u64 offset`, `u64 size`) | from the start of the file; a level holds all its layers, one after another |
| data | per layer of each level: 4x4 BC7 blocks row by row, no swizzle, no padding |

The format and the key are PS5_Lichii's, unchanged: a store made for Lichii works here and the other way round.

## What the emulator does

All in `Eglantine/src/video_core`.

- **Start** (`TextureCacheRuntime`'s constructor, `renderer_vulkan/vk_texture_cache.cpp`): lists the folder into
  `astc_store` (key to path) and logs `Texture store: N BC7 images under <folder>`. Only when the device samples
  BC and has no ASTC of its own.
- **Lookup** (`TextureCache::JoinImages`, `texture_cache/texture_cache.h`, calling
  `TextureCacheRuntime::FindStoredAstc`): a new ASTC image's guest bytes are hashed just before the scan for
  opaque blocks, in the same place and under the same conditions: ASTC is re-compressed to BC3 (the PS5's
  setting), `EDEN_NX2_ASTC_BC1` is not 0, and the data is at most 64 MiB. A found key goes into
  `ImageInfo::astc_store_key` and the opaque scan is skipped.
- **The image** (`renderer_vulkan/vk_texture_cache.cpp`): `IsStoredAstc` makes it BC7 (`AstcHostFormat`, UNORM and
  sRGB views through `StoredAstcViewFormats`, the view format in `ImageView`). It is left out of everything the
  decode paths do: not a huge array, not kept as RGBA8, no trimmed levels, no GPU transcode, no decoder storage
  views, and none of the upload flags (`AcceleratedUpload`, `AsynchronousDecode`), so the uploads by layer and the
  background decode never take it. It counts as BC3 for memory accounting, which is BC7's size.
- **Upload** (`TextureCache::RefreshContents`, calling `TextureCacheRuntime::UploadStoredAstc`): the file's levels
  are read into staging memory and copied into the image. No staging buffer for the guest's bytes, no read of them.
- **Copies between images** (`TextureCache::CopyImage`, `MarkSubresourcesWrittenWhole` in
  `texture_cache/util.cpp`): never between a stored image and one that is not, as their blocks are different
  formats.

### Where this differs from PS5_Lichii

- **A file that does not fit is found out at the lookup.** Lichii checks the file's header at the upload, when the
  image is already BC7, and leaves it empty. Here `FindStoredAstc` reads the header (one open and a 40-byte read
  per found image) and checks size, layers, levels, key and guest length: a file that does not fit is logged and
  the image is decoded from the guest's bytes as before.
- **A second upload checks the bytes again.** The key is taken when the image is made. When the guest writes the
  image's memory later, Lichii uploads the same file again, which is the old texture if a streaming game put
  another one of the same shape at that address. Here every upload after the first hashes the bytes again: the
  new bytes' file is uploaded if the store has one (and fits); if it has none, the image keeps what it shows and
  a warning counts it (`Stored ASTC image ... was rewritten with bytes the texture store does not have`). It
  cannot do better: the host image is BC7 and nothing here encodes BC7. A complete store avoids it.
- **Large files go in pieces.** Lichii reads the whole file into one staging buffer. A large array's file is as
  large as the image, all of it host memory until the GPU has copied it. A file over 32 MiB
  (`EDEN_NX2_TEXTURE_STORE_CHUNK_MIB`) is read in pieces of whole layers of a level up to that size, and each is
  copied by the GPU before the next is read (a wait per piece). Smaller files take one read and no wait.
- **The staging buffer holds only texture data**, so every copy starts on a block boundary. Lichii's copies start
  at the file's offsets, which are 8 bytes off one.
- **The upload flags** (above): Lichii's stored images keep `AcceleratedUpload`.

### Left out

- **The read-ahead** (`vk_texture_store_ahead.h`): Lichii's notes say it brought no fewer slow frames when it was
  measured, and it holds up to 192 MiB.
- **The memo** (`vk_texture_store_memo.h`): it remembers a store key by the hashes of the 64 KiB units an image
  lies in, and those hashes come from Lichii's SSD swap store (`Common::Ps5SwappedUnitHashes`), which this core
  does not have. The class alone would do nothing.
- **Lichii's timing sites** for the stutter trace (`StoreRead`, `StoreUpload` and others): this core's trace has no
  such sites. One log line reports uploads, MiB and time.
- **One folder for all titles**: see above.

## Switches

All read once, when first needed.

| Variable | Default | |
|---|---|---|
| `EDEN_NX2_TEXTURE_STORE=0` | on | No store: the folder is not opened and nothing else in this document happens |
| `EDEN_NX2_TEXTURE_STORE_DIR=<folder>` | the title's folder | The store folder, used as it is |
| `EDEN_NX2_TEXTURE_STORE_CHUNK_MIB=<MiB>` | 32 | The most staging memory one piece of a stored upload takes; 0 reads every file whole, as Lichii does |

Also in the way:

- `EDEN_NX2_ASTC_BC1=0`, or ASTC not re-compressed to BC3 (the `astc_textures=full` dev setting), skips the scan
  the lookup rides on: no texture is looked up.

## What the log says

- `Texture store: N BC7 images under <folder>` at start. Absent: no folder, an empty one, or switched off.
- `Texture store: N ASTC images found, M not (the last: <key>, <bytes> guest bytes, found|not found)`: at the
  1st to 4th, then every power of two, and for every image of 8 MiB or more. The key of a texture that was not
  found is the name its file would need.
- `Texture store: N images uploaded, X MiB in Y ms`: at powers of two and for every file of 32 MiB or more.
- `The texture store's <path> does not fit the ... image with its key`: a file whose header disagrees with the
  image (first 32).
- `The texture store's <key> could not be uploaded`: the file went away or is cut short. The image is left with
  what it had (nothing, the first time).
- `Stored ASTC image ... was rewritten with bytes the texture store does not have`: see above.

## Mario Kart World on the console

PS5 Pro, 2026-11-24, scripted input (title to Grand Prix, the same route every run), 10 minutes a run. The store was
made from the dumps of two such sessions: 8,732 textures, 1.3 GiB of ASTC blocks, 4.7 GiB as BC7 (bc7e "fast",
about 9 minutes on 16 cores; the first session's files a median 52 dB from the decoded ASTC at level 0). Seconds 150 to 590 of each run:

| | FPS | Frames over 100 ms | Worst frame | Peak graphics memory | Found / not in the store | Read from the store |
|---|---|---|---|---|---|---|
| no store (the first 10 minutes of two 30-minute runs) | 32.8, 32.8 | 13, 13 | 0.35 s, 0.48 s | 4,922 and 4,947 MiB | | |
| the store, a file per texture | 29.6 | 94 | 1.7 s | 5,288 MiB | 34,459 / 4,753 | 20.2 GiB in 23.3 s |
| the store, one file for all (tried, not kept) | 28.2 | 154 | 2.4 s | 5,329 MiB | 32,768 / 5,543 | 20.9 GiB in 36.6 s |

What it showed:

- **It works.** Keys taken from the dump are found, BC7 images with these flags are right on this driver, uploads
  in pieces work, and the six screenshots looked at show the races as they should be.
- **It is slower here, by a tenth of the frame rate, with seven times the long frames.** Mario Kart World makes
  about 100 images a second for as long as it runs (59,000 in these 10 minutes), most of them textures it had
  before. Without the store such an image is transcoded on the GPU from the game's memory, which is there. With
  it, each one is read from the console's storage again, on the thread that draws: 20 GiB in 10 minutes for 4.7 GiB
  of files. PS5_Lichii's store was made for a game that loads an area's textures once.
- **A file per texture is not what costs.** One file for the whole store, kept open (an index of key, offset and
  length in front of the files), read no faster; it was taken out again.
- **More graphics memory, not less:** 340 to 410 MiB more at the peak. Of the first session's textures 71% are
  opaque (61% of their bytes), which this core holds as BC1 (4 bits a texel) and the store as BC7 (8 bits).
- **Two sessions' dumps are not a whole store.** The same route a third time still missed one image in eight:
  what the game hands the GPU when an image is made is not the same bytes every session (its data may still be
  arriving). Two images were written again with bytes the store did not have and kept their old texture.

What would have to change for it to pay in this game, none of it done: the game's images made again less often (that costs in either path), the store read off the
drawing thread, and opaque textures held as BC1. The runs wrote 1.3 GiB to the console's storage for the dumps and
9.4 GiB for the store's copies (a file per texture, then one file); all of it is deleted again.

## Untested

The lookup, BC7 images with the flags used here, the upload (whole and in pieces) and the second-upload check
ran with Mario Kart World (above). Still open, in the order of what is most likely to be wrong:

1. **Lichii's tool (`texture_store.py`) has not been run here**, only `from_dump.py`. That a key made from a
   game's files equals the hash the emulator takes rests on Lichii's results with one game on Lichii's core, and
   on the size computation being the same code in both cores (it was compared and is).
2. **Any game but Mario Kart World.** Ocarina of Time, which the store was made for, has not been tried.
3. **Colour.** The dump does not say which channels a shader reads, so `from_dump.py` encodes all four with equal
   weight; Lichii's tool spends the bits on the channels read.
4. **The second-upload check** ran twice in one session (two images kept their old texture); what that looks
   like on screen was not seen.
5. **Other hosts.** The Windows branch of the file reader is there for the core's other builds and was not
   compiled either.
