# Third-party notices

EglantinePS5 is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). It
builds on the following projects, each under its own license.

## Emulator

- **[Eglantine](https://github.com/Djihads80/Eglantine)**, GPL-3.0-or-later, a fork of
  **[Eden](https://github.com/eden-emulator/mirror)**. The emulator core, built from the commit the
  `Eglantine` submodule records, together with the dependencies Eden
  fetches and pins itself: Dynarmic, Boost, fmt, xbyak, zstd, lz4, Opus, SDL3,
  simpleini, sirit, SPIRV-Headers, Vulkan-Headers, Vulkan-Utility-Libraries,
  Vulkan Memory Allocator, ENet, frozen, cpp-httplib, nlohmann/json, and the time
  zone data. Each keeps its upstream license.
- **[Zortos Eglantine fork](https://github.com/zortos293/Eglantine)**,
  GPL-3.0-or-later. Referenced directly by the `Eglantine` submodule, including its
  core, native shader, readback, service, runtime and test changes.
- **[Catch2](https://github.com/catchorg/Catch2)**, BSL-1.0. Used by the focused
  host regression check at the version and source hash pinned by the core.
- **[FFmpeg](https://ffmpeg.org)**, LGPL-2.1-or-later. Built from Eden's pinned
  source with only the H.264, VP8 and VP9 decoders.
- **[OpenSSL](https://www.openssl.org)**, Apache-2.0, and
  **[zlib](https://zlib.net)**, zlib license. Taken from the
  [pacbrew](https://github.com/ps5-payload-dev/pacbrew-repo) PS5 packages.
- **[curl](https://curl.se)** (libcurl), curl license, and
  **[libpsl](https://github.com/rockdaboot/libpsl)**, MIT. From the same pacbrew
  packages; the update check uses them.
- **[PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)**,
  GPL-3.0-or-later. Its update check and self-update kit (`headless/update_check`),
  its self-update helper (`headless/self_update_helper`, built into `self-updater.elf`)
  and `tools/validate-loader-elf.py`.
- **[miniz](https://github.com/richgel999/miniz)** 3.0.2, MIT, unmodified in
  `third_party/miniz` with its `LICENSE`. The self-update helper reads release
  ZIPs with it; the app packs and unpacks save data for the save sync with it.
- **[QR Code generator library](https://github.com/nayuki/QR-Code-generator)** (C), MIT,
  unmodified in `third_party/qrcodegen` with its `LICENSE`. The launcher draws the QR code
  of a save sync's pairing with it.
- **[libnfs](https://github.com/sahlberg/libnfs)** 18.0.0 by Ronnie Sahlberg, LGPL-2.1-or-later (its
  protocol files BSD-2-Clause), in `third_party/libnfs` with its licenses, with the console configuration and
  fixes of [tutifrutas/SwanStationPS5](https://github.com/tutifrutas/SwanStationPS5) (itself from
  [PS5SX2](https://github.com/Swordpdf/PS5SX2) by Swordpdf). The NFS client for games on NFS servers; the
  app's layer over it (`headless/nfs`) is from SwanStationPS5 too.
- **[PS5-NTFS-BROWSER](https://github.com/abcde6543/PS5-NTFS-BROWSER)** by abcde6543, GPL-3.0-or-later. Its
  read-only NTFS reader (`ntfs.c`, `bdev.c`, `part.c`, `utf.c` and their headers, without its writing part) is in
  `headless/ntfs`, with the layer over it (`ntfsfs.cpp`) from SwanStationPS5. It reads the games of an NTFS disk.
- **LLVM compiler-rt** `emutls.c`, Apache-2.0 WITH LLVM-exception.

## Graphics

- **[PS5 Mesa](https://github.com/mihawk-99/PS5_Mesa)** and
  **[PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** by Mihawk. The
  RADV Vulkan driver (Mesa, mainly MIT) and its PS5 link recipe and platform
  layer (GPL-3.0-or-later).
- **[ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan)** by mpereiraesaa,
  an experimental Vulkan graphics and compute API for native PS5 homebrew.
- **[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl)**,
  GPL-3.0-or-later, which includes Mesa components under their own licenses.
  `third_party/app_heap.c` comes from it.

## Platform and interface

- **[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)** by John
  Törnblom (ps5-payload-dev).
- **[PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)**,
  GPL-3.0-or-later. The native app runtime and packaging tool. Its packaging
  tool translates parts of SvenGDK's
  [SharpProspero](https://github.com/SvenGDK/SharpProspero).
- **[PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)** by
  ArkSama, with the cooperative owned-root design from
  **[mpereiraesaa's fork](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)** and the pinned
  **[EglantinePS5 compatibility fork](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)**.
  Lapy is MIT licensed; its shared protocol header is LGPL-2.1-or-later.
  EglantinePS5 pins the fork, invokes its unmodified exact-title helper build,
  verifies its generated manifest and bundles the helper ELF with its license.
- **[ps5log](https://github.com/mpereiraesaa/ps5-agc-gears/tree/1ae1f9182abd2770c131b97419034fb85173c2dc/native/ps5log)**,
  GPL-3.0-or-later. The pinned single-header logging client used as an upstream
  Lapy helper build input.
- **[ProsperoPuzzles](https://github.com/blackbearreloaded/ProsperoPuzzles)**,
  GPL-3.0-or-later. The launcher's drawing, text, animation and sound code
  (`headless/eglantineps5/pe`) started there.
- **Montserrat** by Julieta Ulanovsky and contributors, SIL Open Font License
  1.1 (`third_party/fonts`). The launcher's font is baked from Montserrat Medium.
- **[HarfBuzz](https://github.com/harfbuzz/harfbuzz)**, "Old MIT" license (its
  `COPYING` file). Shapes the launcher's text in the scripts that need it, such
  as Thai and Arabic. Fetched at the release pinned in `tools/deps.json`.
- **[stb](https://github.com/nothings/stb)** by Sean Barrett, MIT or public
  domain (`tools/launcher/stb`). `stb_truetype` bakes the launcher's font and,
  in the app, reads the console's system fonts for the scripts Montserrat does
  not have. `stb_image_write` saves the PC previews and is not part of the app.
- The launcher's sound effects were generated with
  [ElevenLabs](https://elevenlabs.io) and edited for this project.
- `third_party/ps5_pad.hpp` and `third_party/native_audio.hpp` are this
  project's own platform declarations, GPL-3.0-or-later.

## Thanks

EglantinePS5 exists thanks to the Eglantine and Eden maintainers and contributors, Mihawk,
mpereiraesaa, John Törnblom, SvenGDK, and the whole PS5 homebrew community.
