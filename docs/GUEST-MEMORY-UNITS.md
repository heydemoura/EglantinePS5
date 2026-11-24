# Guest memory nothing touches, in 64 KiB units (November 2026)

Steps 2 and 3 of `docs/LICHII-MEMORY-PLAN.md`: what PS5_Lichii (mihawk,
`codeberg.org/mihawk/PS5_Lichii`, commit `5a424f7`) does for all of the guest's memory, done here
for the part of it nothing touches. `EDEN_CRAM_UNITS=0` in `env.txt` switches it off; a slot is
then packed whole with LZ4 or sent whole to the swap store, as in PR #135.

`UnitsOut` and `UnitFill`, the parts that do not need the console's memory calls, are in the host
check (`eden_swap_check`, steps 64 to 67). On the console it ran in four sessions together with
the rest of the plan (two half hours of Mario Kart World, Ocarina of Time, Donkey Kong Bananza):
`docs/LICHII-MEMORY-PLAN.md`, "The console runs", has the numbers. Mario Kart World wrote 870 and
513 MiB in half an hour where it wrote 2.2 to 2.5 GiB with whole slots, at the same frame rate.

## What was there

The guest's memory is in 2 MiB slots (`src/guest_memory_service.inc`). A slot nothing has touched
for a while is Idle; when memory is wanted it is packed whole with LZ4 if that saves 512 KiB, and
if not it goes whole to the swap store, when the budget for writes allows. A touch brings all
2 MiB back. Texture, model and sound data do not pack, so most of what Mario Kart World had to
give up went to the SSD: 2.2 to 2.5 GiB written in half an hour.

## What Lichii does

Its guest memory is backed only when touched, 64 KiB at a time. A cold unit is compressed with
zstd in memory when it comes out at three quarters of its size or less, and goes to the swap
store when not; a unit of zeroes is given back. Lichii reports about 2.7 GB held in 1.1 GB, and
1.4 GB out to the store.

## What is here now

Memory in use stays in 2 MiB slots: the driver takes the pool in 2 MiB pieces, a mapping of that
size is never split on the console, and Lichii lists "2 MiB slab allocation" among what its own
design lacks. It is the Idle slot that goes in units (`PackSlotUnits`). Each of its 32 units is
one of:

| Unit | Where it is | Costs |
|---|---|---|
| zeroes | nowhere | nothing |
| zstd level 1 gives 48 KiB or less | in the slot's piece of the packed store | its packed size |
| found in the swap store by its contents | the store | no write |
| new to the store, and the write budget allows | appended to the store, all of a slot's in one write | 64 KiB written |
| none of these | in memory, in a unit of a slab | 64 KiB |

A slot goes in units only when that frees 512 KiB or more. Its address space becomes a
reservation, and the slot is Packed with `units` set; `swap_units[slot * 32 + unit]` says where
each unit is.

**A touch brings one unit back** (`UnitBack`, from the fault handler): a unit of a slab is
mapped at its address and filled from the packed piece or the store (a unit from the store is
checked against its hash). A read of a unit of zeroes maps the shared zero block read-only; a
write then takes a unit. When all 32 are back the slot is made whole again (`UnpackUnits`) and is
an ordinary slot.

**Slabs.** A slab is a 2 MiB block of the pool cut in 32 units, visible to the service at an
address of its own for as long as it lives. A unit that is awake is also mapped at the guest's
address; a unit that is idle is not, and keeps its memory. A slab whose units are all free goes
back to the pool, and idle units are moved out of nearly empty slabs so that those can
(`SlabCompact`).

**Cooling.** `UnitSweep` runs with the scanner's pass and when memory is asked back. A slot's
awake units are taken from the guest's addresses again once a sample of their contents has stood
still for the time a slot has to (`EDEN_CRAM_PROBE_COLD_S`, 8 s); units idle for long enough are sent
out the way the table above says, the store only under the same budget as before.

**The fastmem window** (`EDEN_FASTMEM64=1`) shows whole 2 MiB blocks at a second address. With
it on, a touch brings the whole slot back.

## What it should change, and what it may cost

- Fewer bytes written. On 1 GiB of what Mario Kart World had sent to the store, 36% of the units
  packed to half their size with zstd: about a fifth fewer bytes written for the same memory
  freed (`~/scratch/swap-tools/ratio.c` at the time; not in the repo).
- A touch on a slot that went out costs 64 KiB of work and memory, not 2 MiB.
- Slots that LZ4 could not pack whole, and the budget would not let out, now give up their
  zero and packable units.
- Cost: a fault per unit where it was one per slot (the report's `back_avg_us` is the time of
  one), and a slot coming back in units is 32 small mappings until it is whole.

The line in the memory report, after the swap store's:
`EDEN_UNITS on= slots= here= idle= slabs= made= packed= stayed= backs= back_avg_us= zero_reads=
woken= evicted= moved= whole=`.

## Open

- Everything that maps (units into a reservation, a slab seen at two addresses, faults on
  several units of one slot from several threads, zstd inside the fault handler) has run in four
  sessions, 350,000 units brought back, and no more: an hour of Mario Kart World and base PS5
  memory are not tried.
- `pack_released` (what packing is counted to have freed) is approximate for slots in units.
- Whether Lichii's packing pass (`EDEN_SWAP_PASS=2`), which ended Mario Kart World sessions early
  with 2 MiB slots, does better with units. Not tried.
