# The BC7 texture store

The PS5's GPU has no ASTC, so the emulator decodes every ASTC texture on the GPU and re-compresses it to BC1/BC3
in the frame that first uses it. The texture store replaces that work for textures converted ahead of time on a
PC: a folder of `<key>.bc7` files, one per texture, where the key is a hash of the bytes the game gives the GPU.
When the emulator makes an image for ASTC data it hashes the data; if the store has that key, the host image is
BC7 and is filled from the file.

Ported from PS5_Lichii (mihawk, codeberg.org/mihawk/PS5_Lichii, commit 5a424f7, GPL-3.0-or-later). Our core and
Lichii's share an ancestor but differ, so this is a port by reading, not a copy.

**The store's path has not run.** It was written without a build; since then the code builds for the console and
that build ran four sessions (`docs/LICHII-MEMORY-PLAN.md`, "The console runs"), but with no store folder for any
game, so every lookup returned at its first line. The layer batches of the transcode did run (Mario Kart World). See "Untested" before relying on any of it.

Without a store folder for the running game the emulator does what it did before: the only new work is one
attempt to open the folder when the game starts.

## Making a store

`tools/texture-store/` holds the offline tool (Python, with a BC7 encoder it builds). Its README says what it
needs, which game it was written for, and what another game takes.

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

## Untested

Everything the store does. In the order of what is most likely to be wrong:

1. **The store's path has not run.** The code builds and the build ran on the console, without a store folder:
   nothing after the first line of a lookup has been executed.
2. **No store exists for any game on this console**, so no lookup has ever matched here. That a key made by the
   tool equals the hash the emulator takes rests on Lichii's results with one game on Lichii's core, and on the
   size computation being the same code in both cores (it was compared and is).
3. **BC7 images with the image flags used here.** A stored image is made like the BC1 images of opaque ASTC
   (format override, two view formats, mutable format), which work on the console; BC7 in their place has not
   been tried on this driver build.
4. **The second-upload check** is not in Lichii and has never run anywhere. It reads the image's guest memory
   again on every re-upload (an upload from guest memory reads it too, so no more than before).
5. **Uploads in pieces** wait for the GPU between pieces, inside the texture cache's upload. The GPU transcode
   waits in the same place already, but this path is new.
6. **Mario Kart World.** The tool was written for Ocarina of Time's BNTX files. Whether Mario Kart World's
   textures can be unpacked to BNTX, pass the tool's checks and give matching keys is unknown; its largest array
   may be over the 64 MiB lookup limit.
7. **A game that streams textures in place** with an incomplete store shows the old texture where the new one is
   not in the store (above).
8. **Memory and time.** Lichii reports a quarter of the memory of RGBA and shorter loads on its build. Against
   this core's BC3 the memory is the same (BC7 is BC3's size, twice BC1's for opaque textures), so the gains to
   expect here are no decode image, no encode and BC7's quality. None of it is measured.
9. **Other hosts.** The Windows branch of the file reader is there for the core's other builds and was not
   compiled either.
