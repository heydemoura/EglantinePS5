# Swap for the guest's memory

Status, 2026-11-23: implemented in `src/guest_memory_service.inc`, on by default, tested with
Mario Kart World and Ocarina of Time on a PS5 Pro. `"performance": { "swap_file": false }` in the
settings file turns it off, as does `EDEN_SWAP=0` in `env.txt`.

Swap here is a store on the console's storage, built the way PS5_Lichii's is: found by its
contents, kept between sessions, never rewritten. (This branch's first design was a file per
session with a ledger of its writes. Its numbers are kept under [Results](#results), as what the
store is compared with.)

Mario Kart World's crashes turned out to have two causes, and the first is one no swap can
cover. Both are described under [Mario Kart World](#mario-kart-world-two-causes).

In short, on a PS5 Pro: Mario Kart World, which ended early on every try, runs 30-minute
sessions at 31 FPS with 2.2 to 2.5 GiB written to the storage (3 of 3; the first half hours of
3 longer sessions wrote 2 to 3 GiB). Without swap the same session ends after 10 minutes. Its first races
(5 minutes), Ocarina of Time and Donkey Kong Bananza write nothing. An hour is at the pool's
edge: one session ran it (7 GiB written, slower in its second half) and one ended at 53
minutes. The store holds 8 GiB at most and starts over once a day at most, so 8 GiB a day is
the most the storage takes: little more than an hour of Mario Kart World, after which its long
sessions end early again for the rest of the day. The numbers are under [Results](#results).

## What it is for

The guest's memory, the driver's GPU memory and Eden's heap share one pool of direct memory
(13,440 MiB with PS5 Pro memory, about 1.2 GB less without). When the pool runs out, an allocation
fails and the session ends.

The guest memory service already packs slots (2 MiB pieces of the guest's memory) with LZ4 when the
pool is short. That gives back only what LZ4 saves. Texture, model and sound data does not compress,
and a game keeps a lot of it once the GPU has its copy. The swap store gives those slots somewhere
to go: the console's internal storage. A slot that goes there gives its 2 MiB block back to the
pool. The next access reads it back.

## The store

The storage's flash wears with every byte written, so the design starts from the writes. The
store is the one PS5_Lichii uses (mihawk, `codeberg.org/mihawk/PS5_Lichii`, `Ps5SwapOpen` and
`Ps5SwapOut` in its `src/common/host_memory.cpp`): the same two files in the same format, with
the same hash and the same lookup, so that a store written by one opens in the other. The
reasons it is cheap on the storage are the same:

- **Found by contents.** `store.bin` in `/data/eglantineps5/cache/swap/` is slots of 64 KiB, and
  `index.bin` the hashes of their contents in slot order. 64 KiB that are to go are hashed, looked
  up, and compared byte for byte with the slot the hash names. What the store holds already costs
  no write, wherever it was in the guest's memory when it was written.
- **Kept between sessions.** A game's own data, loaded as it was the last time, is found there
  again in every later session. (With Lichii's pass, see below: of the 501 MiB one Ocarina of
  Time session wrote, the next found 480.)
- **Never rewritten.** A slot of the store is written once. The store only grows, up to
  `EDEN_SWAP_MIB` (8 GiB), and what a session wrote is what the store grew by.
- **A budget.** At most `EDEN_SWAP_BURST_MIB` at once, refilled at `EDEN_SWAP_RATE_MIB_HOUR`.
- **Room left.** The store does not grow into the last `EDEN_SWAP_KEEP_FREE_MIB` of the storage.

A slot of the guest's memory is 2 MiB here, where a unit of Lichii's is 64 KiB. It goes as 32
units, each found or appended on its own, and a unit of zeroes takes no slot of the store. The
new ones go in one write, side by side: a write costs about 5 ms on the console's storage
whatever its size (see [an hour of Mario Kart World](#mario-kart-world-an-hour)). A slot that
comes back is *Clean*: read-only, and where its units are stays known, so giving its block up
again needs neither a lookup nor a write. Only a write to it forgets that, and then the units
the write left alone are found again by their contents.

Three things are not as in Lichii, each because of a measurement on this port:

- **Slots go when an allocation has failed, not before.** Lichii's packing pass sends every unit
  nothing has touched for a pass and that will not pack. Here that is `EDEN_SWAP_PASS=2`. With
  it Mario Kart World wrote 648 MiB in its first five minutes, where no allocation fails and
  nothing needs to be written; the game then wrote 711 of 824 such slots again within the
  session, the budget was gone when a track's load needed it, and two 30-minute sessions of two
  ended early (after 22 and 29 minutes). A touch brings back 2 MiB here and 64 KiB there, so
  sending ahead of need costs more here than it does there. The pass packs in memory as before;
  a request for memory after a failed allocation sends to the store what the pool lacks.
  Ocarina of Time is the other kind of game: with the pass writing it kept its frame rate, and
  its second session had 0.9 GB of its memory in the store, for 501 and 497 MiB written in the
  two sessions. It does not need that memory in any run made here, so nothing is written for it
  either.
- **1 GiB at once and 8 GiB an hour**, where Lichii has 512 MiB and 4 GiB. With those and slots
  going on request only, a Mario Kart World session ran out of budget at its second large load
  and ended after 29 minutes 48 seconds.
- **A store that is nearly full starts over** at the next launch, once a day at most: one with
  less than a quarter of its size left (`EDEN_SWAP_START_OVER_HOURS`; `started` in its folder
  holds when it last did). Lichii's takes nothing new once it is full. Mario Kart World's memory
  differs from session to session (a third of what a session sends is found again in the next),
  so it fills 8 GiB in little more than an hour of play and would then have no store left to
  send anything new to. A session that starts with less than a quarter of the store left is one
  that ends at its next large need, which is why the store does not wait until it is full.

The memory step before the store is this port's own too: LZ4 on a whole slot, kept in memory at
60% or less, where Lichii has zstd on each 64 KiB unit, kept at 75% or less. What that would
change is measured under [What could lower the writes further](#what-could-lower-the-writes-further).

From this branch's first design, because they choose what goes:

- **Memory first.** A slot that compresses to 60% or less goes to the packed store in memory.
- **Only slots nothing touches.** While the pool is short the scanner takes all access away from
  slots that have not been written for 8 s (the *Idle* state: the block stays, only its protection
  changes). A slot the game or the emulator touches faults once and is left alone for longer each
  time, up to 40 s. Only slots still Idle after 2 s are taken. When there are none, the request
  gets nothing: the game's working memory never goes to the store.
- **Only what the pool lacks.** A request names all its taker needs: the driver asks for the
  whole 560 MiB of one texture array when Mario Kart World loads a track, and again at each of
  its tries. Only what the pool does not have free is taken from the guest (no less than 64 MiB).
- **Memory the game reuses goes last.** The scanner counts each time a slot is written again
  after 30 s or more unwritten. Slots never counted go first, the longest unwritten first; the
  others only when a request would otherwise get nothing (`EDEN_SWAP_LAST_RESORT=0`: never).
- **The keeper** holds 512 MiB of the pool free with what costs no write: slots that compress,
  and slots whose bytes the store holds.

| Switch (`env.txt`) | Default | Meaning |
|---|---|---|
| `EDEN_SWAP` | `1` | `0`: no swap store (also `"swap_file": false` in the settings file) |
| `EDEN_SWAP_MIB` | 8192 | Most the store may hold |
| `EDEN_SWAP_BURST_MIB` | 1024 | Most that may be written at once |
| `EDEN_SWAP_RATE_MIB_HOUR` | 8192 | Refill of the burst, per hour |
| `EDEN_SWAP_START_OVER_HOURS` | 24 | A store with less than a quarter of its size left starts over at a launch this long after it last did; `0`: never (Lichii's way) |
| `EDEN_SWAP_KEEP_FREE_MIB` | 1024 | Storage space the store does not grow into |
| `EDEN_SWAP_PASS` | 0 | `2`: the scanner's pass sends slots to the store ahead of need, with writes (Lichii's way); `1`: only those whose bytes the store holds |
| `EDEN_SWAP_LAST_RESORT` | 1 | `0`: memory the game reuses is never sent, and such a request gets nothing |
| `EDEN_SWAP_KEEPER` | 0 | `1`: the keeper may write too, once the pool has stayed short for 3 s |
| `EDEN_SWAP_LOG` | 0 | `1`: one `EDEN_SWAP_PASS` line per scanner pass |
| `EDEN_CRAM_KEEP_FREE_MIB` | 512 | Free pool the keeper holds; `0`: no keeper |
| `EDEN_CRAM_IDLE` | 1 | `0`: no Idle slots (packing chooses by writes, and no swap) |

Deleting `cache/swap/` while no game runs is always safe: the next session starts an empty store.

### What that means for the storage

The store is 8 GiB at most, each of its slots is written once, and it starts over once a day
at most. So the storage takes at most 8 GiB for each time the store starts: 8 GiB a day over
any stretch of days (16 GiB within one day when a store fills up, starts over and fills up
again), 3.1 TB a year for a console that fills it every single day. The console's internal drive
is 825 GB to 2 TB of flash, depending on the model. Sony does not publish its endurance;
consumer TLC drives of about 1 TB are commonly rated near 600 TBW, of which that is about 0.5%
a year.

Measured: Mario Kart World writes nothing in its first five minutes, 2.2 to 2.5 GiB in 30
minutes and about 7 GiB in an hour; Ocarina of Time and Donkey Kong Bananza write nothing. Half
an hour of Mario Kart World every day is about 0.9 TB a year. The store itself takes up to
8 GiB of the storage for good, where the first design's file took 2 GiB at most and only while
a game ran.

## How it is kept safe

- A lookup compares bytes: neither two contents with one hash nor an index that a power cut left
  ahead of its store can pass for a slot's data. A slot of the store counts only when both its
  bytes and its hash are there.
- A slot of the guest's memory is hashed whole when it goes and checked when it comes back
  (Lichii trusts the store there). Data that does not match ends the session with a line in the
  log; it never reaches the game. (24,795 slots were read back in the test runs of the first
  design and 28,430 in those of this store; none failed.)
- A write that fails or is cut short (storage full, an I/O error) leaves the slot as it was, with
  its block, and ends swapping for the session.
- No slot of the store is ever rewritten while a session runs, so what a fault handler reads is
  never being written: it reads without any lock against the one writer.
- No slot of the guest's memory is ever readable half-written or half-read: it is locked while it
  changes, has no access until its data is complete, and is filled in a mapping of its own before
  it is shown.
- The system never reads guest memory on the app's behalf. A slot that is Idle or in the store has
  no access, and a system call given a pointer into it (a file write, a socket send) would fail
  instead of faulting it back in. The input buffers of the guest's service requests are now always
  copies made by the emulator's own read (`hle_ipc.cpp`), and slots being written are never made
  Idle.
- Without storage access (`/data` not reachable) there is no swap store.
- The bound on the writes is only as good as the time in `started`. It is written, and synced,
  before the store is emptied; a store whose time cannot be written does not start over; and a
  time from after now (a console that lost its clock, or had it ahead) is left as it is and
  waited for, since a clock that is wrong at every other launch would otherwise empty the store
  at every other launch. Deleting `cache/swap/` is the way out of a time far ahead.
- What the storage has free is asked once a scanner pass. When that cannot be told (the folder
  was deleted under a running game, say), the store does not grow.

## Mario Kart World: two causes

On `main` Mario Kart World ended early on every try (48 s after launch in this set's run, and
in all 6 runs of another series the day before). Two things were behind that, and swap is the
answer to the second one only.

### The render resolution, changing every frame

Mario Kart World chooses its render resolution frame by frame, from how long the GPU took for
the last frames. That is what filled the pool, and swap cannot fix it: it fed it.

What happened, measured with `EDEN_NX2_TC_SHAPES=1` and `EDEN_NX2_MEM_TRACE=1`:

1. A frame comes late. Any cause will do: a request for memory that packs slots while the frame
   waits, slots read back from the storage, the game streaming the next part of a track.
2. The game lowers its render resolution, then changes it again every frame: 1616x900, 1604x900,
   1608x904, and so on. Every size needs its own set of render targets, at new addresses.
3. The texture cache makes them: 25,000 to 28,000 images in 600 frames instead of 1,000 to 5,000,
   in 3,000 to 4,000 different shapes. GPU-written images go from 0.48 GB to 1.2-2.2 GB, the
   driver's memory from 4.5 GB to 6-7.6 GB, within seconds.
4. The pool runs out. Allocations wait for memory, frames take 60 to 120 ms with single frames of
   1 to 5 s, the resolution keeps changing, and the state holds itself up until something fails.

It started in three places: at the main menu (the world is drawn behind it), in the first seconds
of a race, and about 70 s into a race, when the next part of the track streams in.

The fix is the GPU clock, a setting the core already has: how much shorter the GPU's time for a
frame looks to the game. With boost (the core's default) it looks 4 times shorter, with overclock
16 times. The port adds a third step, fixed: 256 times (`gpu_clock_divisor`,
`headless/performance.h`, read in `GPU::GetTicks`).

| Mario Kart World, PS5 Pro, GPU clock | Runs | Ended early |
|---|---|---|
| boost, `main` | 1 (and 6 the day before) | all (this one after 48 s) |
| boost, with swap (the first design's variants while it was made), 5 minutes | 21 | 13; 3 more ran at 12 to 22 FPS |
| overclock, with swap, 5 minutes | 9 | 0 |
| overclock, with swap, 10 to 30 minutes | 7 | 6, after 8 to 17 minutes |
| fixed, with swap, 30 minutes or more | 17 | 6, none of them this way (see [Results](#results)) |
| fixed, no swap, 5 minutes | 2 | 0 |
| fixed, no swap, 30 minutes | 1 | 1, after 10 minutes (the pool ran out, see below) |

Overclock holds the resolution through frames of up to about 270 ms. That is enough for a race.
In a session a longer frame comes sooner or later: a request for a 128 MiB staging buffer that
waited 300 ms for memory set the loop off 9 and 14 minutes into two sessions, and both ended
within the minute. With fixed, frames of 1 to 12 s (allocations waiting for memory late in a
session) did not: the cache made at most 8,914 images in 600 frames.

Mario Kart World gets fixed unless the settings file says otherwise
(`"performance": { "gpu_clock": "normal" | "boost" | "overclock" | "fixed" }`, for every game or
under `"games"` for one; `EDEN_GPU_CLOCK` in `env.txt` goes before both). Other games keep boost.
The game then renders at its full resolution all the time. On a PS5 Pro that costs nothing that
could be measured: the 5-minute runs with overclock (33.3 to 35.9 FPS) and with fixed (34.4 to
35.5) are in the same range.

### A session that needs more than the pool has

With the GPU clock fixed and no swap, Mario Kart World runs its first races: 2 of 2
five-minute runs at 34 FPS, with no allocation refused at all. The pool is still too small for a
session. Each track brings its own textures, among them arrays the emulator has to decode (one
of 1434x1434 with 46 layers takes 560 MiB of graphics memory), and what the game holds grows
with every race. Ten and a half minutes into the session without swap an allocation found the
pool empty (4 MiB free of 13,440), 99 of 118 requests for memory had come back with nothing,
and the session ended. With swap the same session runs its 30 minutes; see
[Results](#results).

## What else changed, and why

Testing swap showed faults that are not about swap itself. Each ended a Mario Kart World
session on its own.

1. **Memory asked back was counted but not given back.** A recovery told the driver "32 MiB back"
   while the blocks had gone to refill the service's own stock for unpacking. The driver's second
   try found nothing. A recovery now gives blocks straight to the pool and counts only those.
2. **One retry.** The driver asked for memory back once and tried once more. By then another taker
   often had it. Buffers and images are now tried again for up to 2 s after asking memory back
   (`RetryAfterRecovery`, `vulkan_memory_allocator.cpp`), the heap for up to 2 s, and a guest
   write that waits for a block asks for one itself instead of only waiting for the caches.
3. **A mapping taken away under its writer.** The heap gave memory back with `munmap`, then reserved
   the range again. In between, a mapping another thread asked for without a fixed address could
   land there, and the reservation then removed it. The guest memory service unpacks and zeroes
   blocks through such mappings, inside fault handlers. A fault there ends the app with no crash
   report: this was the "silent exit" at Mario Kart World's second race load. Ranges are now
   reserved in one call over the mapping (`ReserveOver`), and the service's views have address space
   of their own (`MapView`).
4. **The game's working memory was packed.** A recovery took slots unwritten for 10 s. Unwritten is
   not unread: the game's threads faulted them back in at once (20,000 unpacks in two minutes) and
   frames took three times as long. A recovery now takes only Idle slots.
5. **Memory was asked back by the thread that draws.** Each request packed or wrote slots while a
   frame waited. A keeper thread now looks at the pool five times a second and frees what costs no
   write before an allocation has to ask.
6. **A block in one piece, asked of 2 MiB blocks.** The heap wanted an 88 MiB block in one piece
   when the next part of a track streamed in. The pool had none, and guest memory was packed and
   written to the storage twenty times over while the thread waited, although no amount of 2 MiB
   blocks makes one 88 MiB block. The heap now takes such a block in pieces at once.
7. **The packed store kept its blocks.** A block of the packed store went back only when all of its
   data had. Ten minutes into a session the store held 544 MiB of blocks for 282 MiB of data. Data
   in blocks less than half full now moves together (`StoreCompact`).
8. **A taker that did not ask.** The emulator's sparse tables (the GPU's page tables among them)
   and the guest's memory when it is handed out take their 2 MiB pieces in `CommitSparsePage`,
   which ended the app at the first refusal. 21 minutes into a session a piece of the GPU's page
   tables found 1 MiB free, with 1 GB of Idle slots and most of the swap budget unused (1 of the 4
   thirty-minute sessions run after the keeper stopped writing to the storage; while it wrote, it
   kept more of the pool free). The commit now asks for memory back and is tried again for up to 2 s
   (`CommitWithRecovery`, `memory_pages.cpp`; `EDEN_SPARSE_COMMIT_TEST=N` refuses the first try
   of every N-th commit, to test that path: with 20, a 5-minute session ran at 35.0 FPS and each
   refused commit had its memory at the next try). No session has ended that way since.
9. **Tries that were gone in 20 ms.** A graphics allocation that found no memory asked for some
   back and tried again, 40 times, and waited 50 ms before the next try only when nothing had
   come back. 41 minutes into a session each request brought 2 MiB (what the try before it had
   left behind), 32 tries were used up in 20 ms and the other 8 in 0.4 s, and a render target
   went without memory, at a load where the images before it had theirs after up to 18 tries.
   The tries now go by the clock: for 4 s, with a wait after every try that failed
   (`RetryAfterRecovery`, `vulkan_memory_allocator.cpp`), or until 8 requests in a row brought
   all they asked for.
10. **A render target without memory ended the session.** The texture cache leaves an image
    that got no memory out and makes it again later, but handed the framebuffer the null view
    for it, and the driver read through a null handle (the invalid access that ended two of the
    sessions in the tables below, a second after `No memory for the 1920x1076x1 image`). The
    target is now left out of that draw, as upstream Eglantine does since its #1766 (`043c2ce162`).

## Results

A PS5 Pro (the pool is 13,440 MiB) unless said otherwise, release builds with scripted input.
Mario Kart World at 1x: from the title into a Grand Prix, race after race; the first race starts
about two minutes after launch. FPS and frames over 100 ms are counted from 2:30 on. "Written"
is what went to the storage in the session, "found" what went to the store without a write.

### Mario Kart World, 5 minutes

| | Runs | Ended early | FPS | Frames over 100 ms | Written |
|---|---|---|---|---|---|
| `main` | 1 (and 6 the day before) | all (this one after 48 s) | | | |
| the store, empty | 1 | 0 | 33.7 (while 1 GiB was read from the console over FTP) | 0 | 0 |
| the store, after two 30-minute sessions | 1 | 0 | 34.1 | 0 | 0 |
| the store with Lichii's pass (`EDEN_SWAP_PASS=2`), empty | 1 | 0 | 35.2 | 1 | 648 MiB |
| the first design | 6 | 0 | 34.4 to 35.5 | 0 to 1 | 0 |
| `EDEN_SWAP=0` | 2 | 0 | 34.1 to 34.2 | 0 to 1 | 0 |

No allocation was refused in any of these: the first races fit once the render resolution
holds still, and nothing has to be written. Only Lichii's pass writes, ahead of a need that
does not come.

### Mario Kart World, 30 minutes

The store:

| | Store at launch | Outcome | FPS | Frames over 100 ms | Worst frame | Written | Found | Slots written | Dropped by a later write |
|---|---|---|---|---|---|---|---|---|---|
| on request, 1 GiB and 8 GiB an hour, 1st session | empty | ran | 31.5 | 117 | 3.8 s | 2,468 MiB | 486 MiB | 1,300 | 629 (48%) |
| the same, 2nd session | 2,468 MiB | ran | 31.0 | 112 | 5.8 s | 2,229 MiB | 1,236 MiB | 1,233 | 668 (54%) |
| the same on another store, 2nd session | 1,743 MiB | ran | 31.2 | 121 | 4.5 s | 2,152 MiB | 985 MiB | 1,193 | 613 (51%) |
| on request with Lichii's budget, 1st session | empty | ended at 29:48 (out of device memory) | 31.5 | 107 | 3.8 s | 1,717 MiB | 448 MiB | 924 | 521 (56%) |
| Lichii's pass and budget, 2nd session | 654 MiB | ended at 22:15 (a render target without memory) | 30.1 | 100 | 6.1 s | 1,924 MiB | 865 MiB | 1,003 | 775 (77%) |
| Lichii's pass and budget, 3rd session | 2,610 MiB | ended at 29:17 (out of device memory) | 29.6 | 173 | 10.1 s | 2,409 MiB | 1,460 MiB | 1,356 | 1,045 (77%) |

With slots going on request and the budget at 1 GiB and 8 GiB an hour, 3 of 3 sessions ran
their 30 minutes, at 31.0 to 31.5 FPS with 2.2 to 2.5 GiB written (the first design, in the
last two rows of the table below: 30.5 to 31.2 FPS and 2.3 to 2.8 GiB), and so did the first
half hours of the sessions under "an hour" below. A second session found a third of what it
sent in the store already (36% and 31%; a first session finds 16%, what it sent before itself)
and wrote as much as a first one all the same: what the store saves this game is within the
spread between sessions.

The sessions with Lichii's budget or pass ended for want of memory at a load. With the pass the
budget was spent ahead of need: 77% of the slots written were dropped by a later write of the
game's, where it is about half when slots go on request.

The first design (a file per session), for comparison:

| | Outcome | FPS | Frames over 100 ms | Worst frame | Written | Slots written | Dropped by a later write |
|---|---|---|---|---|---|---|---|
| `EDEN_SWAP=0` | ended at 10:29 (out of device memory) | 30.7 | 21 | 12.6 s | 0 | | |
| the keeper writes, copies ahead (as first built) | ran | 30.8 | 116 | 11.1 s | 3,429 MiB | 2,139 | 1,594 (75%) |
| + the count of reuse, last resort | ran | 30.9 | 111 | 7.5 s | 3,762 MiB | 2,326 | 1,630 (70%) |
| on request only, no copies | ran | 30.7 | 116 | 5.0 s | 2,757 MiB | 1,705 | 948 (56%) |
| + only what the pool lacks, 3 runs | 2 ran; 1 ended at 21:14 (a table's commit, see above) | 30.5, 31.2 | 129, 124 | 4.2 s, 4.8 s | 2,287 and 2,765 MiB | 1,414, 1,717 | 546 (39%), 896 (52%) |
| + commits that ask, 2 runs | ran | 30.6, 30.8 | 148, 128 | 5.7 s, 5.2 s | 2,728 and 2,490 MiB | 1,681, 1,533 | 864 (51%), 708 (46%) |

A long session of this game needs more than the pool holds: at the end of the sessions that
ran, 1.0 to 1.5 GB of its memory had their bytes on the storage only. Most of what it can give
up is memory it reuses, which is why about half of the slots are dropped by the game's own next
write however they are chosen. What the choices changed is how much is written for the same 30
minutes; the frame rate is the same in all of them. Writing on request only is what made the
difference, in the first design and again with the store.

What is written goes at a few loads. In the first session of the first table, 722 MiB had been
written after 21 minutes; 800 MiB went within the next minute and 730 MiB in the session's last
one, each time for a track's textures (the 560 MiB array and a 128 MiB staging buffer among
them), which found the pool empty and nothing left to take but memory the game reuses (826
slots as a last resort, two thirds of the slots written). At the first of those loads 30
requests in a row found no slot left to take at all. `EDEN_SWAP_LAST_RESORT=0` leaves such
requests empty-handed from the start; some allocations can do without (an image is not made),
others end the session, as the run without swap shows.

### Mario Kart World, an hour

Each from an empty store. (The 30-minute sessions above were run before items 9 and 10 above
and before a slot's new units went in one write; so was everything in the sections below but
the runs without PS5 Pro memory, which are of the code as it is.)

| | Outcome | FPS | Frames over 100 ms | Worst frame | Written | Found | Time in writes |
|---|---|---|---|---|---|---|---|
| before items 9 and 10 above | ended at 41:28 (a render target without memory) | 30.4 | 202 | 5.2 s | 4,104 MiB | 1,463 MiB | 19 s at 40:40 |
| with them, and a write for each run of a slot's new units | ran | 28.4 | 377 | 29.1 s | 6,934 MiB | 3,376 MiB | 199 s |
| as it is now: one write for a slot's new units | ended at 53 minutes (a GPU protection fault) | 29.9 | 268 | 6.0 s | 5,500 MiB | 2,387 MiB | 44 s |

The last two, by ten minutes (a write for each run / one write):

| Minutes | FPS | Worst frame | Written by then | Speed of the writes |
|---|---|---|---|---|
| 2 to 12 | 31.6 / 31.0 | 1.7 s / 2.5 s | 0.4 / 0.4 GiB | 196 / 161 MiB a second |
| 12 to 22 | 30.8 / 30.6 | 2.4 s / 4.1 s | 1.1 / 1.5 GiB | 190 / 185 |
| 22 to 32 | 29.4 / 29.2 | 4.8 s / 3.5 s | 3.0 / 2.8 GiB | 146 / 190 |
| 32 to 42 | 27.3 / 29.9 | 6.7 s / 6.0 s | 4.0 / 4.1 GiB | 41 / 157 |
| 42 to 52 | 27.3 / 28.7 | 29.1 s / 5.9 s | 5.8 / 5.4 GiB | 27 / 66 |
| 52 to 60 | 23.0 / (ended) | 13.7 s | 6.8 GiB | 11 |

The second half of an hour costs more than the first. What the game holds grows with every
race, each load takes more from memory the game itself reuses (86% of the slots written in the
hour were dropped by a later write of the game's), and more than half of what an hour writes
is written in its second half. An hour is about 7 GiB: the store's 8 GiB are little more than
an hour of this game, after which its long sessions end early again until the store starts over
the next day.

One write for a slot's new units is what keeps that second half playable. Late in a session the
store has most units of a slot and the new ones lie scattered among them; each run of them was a
write of its own, and a write costs about 5 ms on this storage whatever its size. In the last
eight minutes of the hour that ran, 971 MiB took 85 s to write, and its second half had frames
of 9 to 29 s while loads waited for writes. With one write a slot that part of a session is
written at 66 to 157 MiB a second, and no frame took longer than 6 s.

The session that ended at 53 minutes did so with a GPU protection fault: the GPU read a page in
the driver's 32-bit window (descriptor sets, shader code) that was not mapped, and the system
ended the app. No allocation had been refused in the minute before. It is the only such fault
in the runs kept, and its cause is not found. A session of this game close to an hour is at the
edge of what the pool holds in its 2 MiB pieces; what moves that edge is less graphics memory
for the game (see [What could lower the writes further](#what-could-lower-the-writes-further)),
not more swap.

### The Legend of Zelda: Ocarina of Time

| | Runs | FPS | Frames over 100 ms | Written |
|---|---|---|---|---|
| the store, 3 minutes | 3 | 33.2 to 33.4 | 0 | 0 |
| the store with Lichii's pass, 1st and 2nd session | 2 | 33.5, 33.3 | 0 | 501 and 497 MiB (480 MiB found by the 2nd) |
| the first design, 3 minutes | 6 | 33.5 to 34.2 | 0 | 0 |

The pool gets short (its largest free block goes down to 1.0 GB, and 3.0 GB of the game's memory
sit Idle), no allocation is refused, and nothing is written. `main` ran the same scene at 33.2
and 33.7 FPS (2 runs; by the game's frames, which gives 33.5 for the store's runs and 33.6 to
34.2 for the first design's six). With its pipeline cache removed (the game's own squeeze used
to be the heap while pipelines compile at the scene's load) it was the same with the first
design: 33.4 and 33.8 FPS, 33.1 on `main`, the pool's largest free block never under 0.8 GB,
nothing written. In none of these runs did Ocarina of Time need swap.

### Donkey Kong Bananza

Two 3-minute runs with the store: 30.6 and 30.1 FPS (the first design: 30.5, `main`: 30.2). The
pool never gets short (its largest free block stays above 6 GB), so the store is not even
opened.

### Without PS5 Pro memory

The same console with the app's memory mode of a PS5 (12,224 MiB: `attribute3` `0x80040` in
`param.json`).

| | Runs | Outcome | FPS | Frames over 100 ms | Worst frame | Written |
|---|---|---|---|---|---|---|
| Mario Kart World, 5 minutes, the store (1st and 2nd session) | 2 | ran | 33.7, 34.0 | 13, 6 | 0.98 s, 0.29 s | 597 and 297 MiB (12 and 216 MiB found) |
| Mario Kart World, 30 minutes asked for, the store | 1 | ended at 10:40 | 30.0 | 68 | 12.4 s | 2,255 MiB |
| Ocarina of Time, 3 minutes, the store | 1 | ran | 33.3 | 0 | 0.07 s | 0 |
| Mario Kart World, 5 minutes, the first design | 2 | ran | 33.8 to 34.9 | 6 to 8 | 0.39 s | 433 and 479 MiB |
| Ocarina of Time, 3 minutes, the first design | 1 | ran | 33.7 | 0 | 0.06 s | 0 |

With 1.2 GB less, Mario Kart World's first race needs the store, and its second session finds
there what the first one wrote: half the writes for the same five minutes. (On 2026-11-07 a
build of that day ended 4 of 4 times in this mode, 80 to 154 s into the session.)

A longer session does not fit the budget in this mode. It had written 2.3 GiB after ten and a
half minutes, the budget was used up (4,781 slots refused), a 128 MiB staging buffer found 28
MiB free, and a read of the guest's memory that could get no block for its slot ended the
session. `EDEN_SWAP_BURST_MIB` and `EDEN_SWAP_RATE_MIB_HOUR` buy it minutes at that price, 13
GiB an hour, and the store's 8 GiB would be full after forty of them: on a PS5 without Pro
memory Mario Kart World stays a game of short sessions until it needs less memory.

### A store that fills up, and a game started again

With the store limited to 256 MiB (`EDEN_SWAP_MIB=256`) and Lichii's pass, so that it fills
within minutes: the first session wrote 255 MiB and then left 43 slots where they were
(`full=43`). A second session the same day found 102 MiB of what it sent there and wrote
nothing. With `started` set 25 hours back, the third said `(it was full and starts over)` and
wrote 255 MiB again. All three ran their five minutes, at 33.5 to 34.8 FPS: nothing needs the
store that early. A session that needs it and finds it full ends at its next large need, as the
session with Lichii's budget did.

A game left with Touchpad + L1 and started again from the launcher, in the same process (with
Lichii's pass again, for writes in both): the first had written 1,102 MiB when it was left, with
418 MiB of its memory in the store. The second found 269 MiB of what it sent there already,
wrote 871 MiB, and had 295 slots read back, all as they went.

### What the tests wrote to this console

About 96 GiB in two days: 58.6 GiB by the first design's runs (its ledger's `written_ever`
after the last of them), most of it by the variants that wrote ahead of need, and about 37 GiB
by the store's (30 runs, 9 of them sessions of 22 minutes to an hour). The test stores are
deleted.

## What was tried and dropped

- **A file per session, with a ledger** (this branch's first design). Slots went to
  `guest-memory.swap` in the cache folder, unlinked while open and 2 GiB at most, compressed with
  LZ4 where that saved a tenth. A ledger beside it counted what the session, the day and the
  console's life had written (4 GiB a session and 8 GiB a day at most). It ran Mario Kart
  World's 30 minutes, with the numbers above. It wrote about as much in such a session as the
  store does (2.3 to 2.8 GiB, the store 2.2 to 2.5), so that is not what it was replaced for.
  The store keeps what it holds for later sessions (a game whose memory is the same every time
  writes it once), sends only the 64 KiB units of a slot that changed, and bounds the writes
  by its size where the ledger counted them.
- **Lichii's pass as the default**, and **Lichii's budget**: both under [The store](#the-store),
  with the sessions that ended early in the table above.
- **A store that starts over whenever it is full.** Mario Kart World fills 8 GiB in little more
  than an hour of play, so a day of it would have written the store several times over. Once a day
  bounds what the storage takes; the price is that Mario Kart World's long sessions end early
  for the rest of a day in which the store has been filled.
- **Writing ahead of need** in the first design (slots idle for 30 s went to the file while the
  pool had less than 3 GiB free). The race then started with the room it needs. But 1 GB had
  gone to the file, and the game wanted 800 MiB of it back within seconds when it streamed the
  next part of the track. 3 of 3 runs ended worse than without it, with 2.0 to 2.3 GiB written
  each. Lichii's pass, tried on the store later, is the same idea and went the same way.
- **Taking slots that are only unwritten** when the idle ones run out. It fed the state described
  under Mario Kart World instead of ending it: 0.7 to 2.0 GiB written per run for nothing.
- **The keeper writing** to hold 512 MiB free at all times. In a 30-minute Mario Kart World
  session it had most of 3 GiB written, two thirds of it for slots the game wrote over within
  minutes, while requests after a failed allocation came 42 times. Free memory also draws
  takers: the graphics caches keep more when the pool has more. (`EDEN_SWAP_KEEPER=1` still
  does it.)
- **Copies made ahead** for slots that are read and not written (while the slot stays, so that a
  busy moment can take it for nothing). 248 of 280 copies were dropped by a write: early in a
  session nothing tells memory the game keeps from memory it has not reused yet.
- **Letting reused memory go after 2, 4, 8, then 16 minutes unwritten.** Of the slots written
  after one and two such counts, 74% and 83% were dropped by the game's next write.

## What could lower the writes further

- **The texture array.** The largest single need is a decoded texture array per track:
  1434x1434, 46 layers, 560 MiB as RGBA8, asked for in one piece. Late in a session most of
  what is written is written to make room for it and for its 128 MiB staging buffer (in the
  2nd session on another store above, 729 MiB went to the store within the 30 s of one such
  load, and then seven smaller requests found no slot left to take). Keeping that array
  compressed for the GPU is a change to the texture cache, not to swap, and would save more
  than anything left in the swap policy. (Upstream Eglantine backs Mario Kart World's oversized
  lightmaps at lower detail since its #2459; whether that is this array was not looked into.)
- **Lichii's memory step.** There a unit stays in memory when zstd (level 1) brings it to 48 of
  its 64 KiB or less, and goes to the store otherwise. Here a slot stays in memory when LZ4
  brings all of its 2 MiB to 60% or less. Measured on what Mario Kart World had the store hold
  (1 GiB of a 3.9 GiB store, in four parts): zstd would have kept 36% of the units in memory
  (27% to 48% by part) at about half their size, LZ4 with the same limit 29%. For the same
  memory given back that is about a fifth fewer bytes written with zstd, a sixth with LZ4. The
  price: a quarter more slots have to be taken for the same memory, at the loads where there
  are no more slots to take as it is, and a slot then comes back from two places. Not done.

## Reading the log

`EDEN_SWAP on: /data/eglantineps5/cache/swap holds 4698MiB from earlier sessions, may hold
8192MiB; burst=1024MiB rate=8192MiB/h` is printed when the pool first gets short and the store
is opened. `(it was full and starts over)` after the size says that this launch emptied it.

A crash report has a line for it: `swap: on, the store holds 8100 MiB of 8192, 2152 MiB written
this session, 17 slots refused by the budget and 3 by the store's size`. A session that ended
for want of memory with slots refused is one that a larger budget (`EDEN_SWAP_BURST_MIB`,
`EDEN_SWAP_RATE_MIB_HOUR`) or a store with room would have carried further.

`EDEN_SWAP now on=1 (ok) store=5500MiB out=900MiB clean=36MiB written=5500MiB found=2387MiB
zeroes=70MiB write_ms=43819 writes=3112 reused=1546 dirtied=3428 reads=5017 read_avg_us=2928
read_max_us=84831 denied_rate=0 full=0 absent=8269 idle_now=1934MiB idled=120550 woken=96306
kept_free=18822MiB/9901 last_resort=2534 late_reads=0/0`

- `store`: what the store holds, earlier sessions' slots included.
- `out`: guest memory whose only copy is in the store now. `clean`: resident, with its bytes
  there too.
- `written`: what this session added to the store, which is what the storage took.
- `found`, `zeroes`: what went without a write, the store holding it already or it being zeroes.
- `write_ms`, `writes`: time spent in those writes, and how many they were (one for a slot's new
  units). `reads`, `read_avg_us`, `read_max_us`: slots read back.
- `reused`: Clean slots that gave their block up again with neither a lookup nor a write.
- `dirtied`: slots a write of the game's took out of the store's keeping.
- `denied_rate`, `full`: slots that stayed because the budget or the store's size said no.
- `absent`: slots that could have gone without a write and were not all there.
- `idle_now`, `idled`, `woken`: Idle slots now, made, and touched again.
- `kept_free`: what the keeper freed before any allocation failed, and in how many passes.
- `last_resort`: slots the game reuses that went because a request had nothing else.
- `late_reads`: read faults that found their slot back already, and those that faulted again.

`EDEN_SWAP_AGES now written=a/b/c/d/e/f dropped=… by_rewrites=g/h/i/j dropped=… slots=k/l/m/n`:
the slots written by how long they had been unwritten (under 30 s, 60 s, 2, 5, 10 minutes, more)
and by how often they had been written again before (never, once, twice, more), each with how
many a later write dropped; then all slots that hold data, by the same count.

`EDEN_CRAM_PACK recover asked=… pool_free=… packed=… idle_blocks=… to_swap=… last_resort=…
swap_written=… candidates=…` is one request for memory back after a failed allocation: what was
asked, what the pool had free then, what went back to the pool from slots and from the service's
idle blocks, how much of it by way of the store and how much of that was written, and how many
Idle slots there were to take.

`EDEN_SWAP_PASS free=… packed=… store=… written=… found=… tokens=…` (with `EDEN_SWAP_LOG=1`) is
one pass of the scanner: what the pool has free, slots packed by the pass, the store's size,
the session's writes and finds, and what the budget has left.

`EDEN_SPARSE_TABLES a commit found no memory and got it after asking (N so far, try T)` says that
a table's or the guest's next 2 MiB found the pool empty and had memory given back (printed the
1st, 2nd, 4th, 8th ... time).

`EDEN_NX2_TC_SHAPES=1` (with `EDEN_NX2_TC_LOG=1`) lists the shapes of the images the texture cache
made in each 600 frames. `EDEN_NX2_MEM_TRACE=1` now also prints the allocator's pressure counters,
the collector's margins, and what the cache holds unused.
