# Selevet / Mario Kart World experiment

Read [the findings](../../docs/SELEVET-MKW-EXPERIMENT.md) before applying
`ps5-port.patch`. This is an optional investigative snapshot with known loading
stalls and an allocation failure. It includes temporary MKW launch/probe code.
The normal checkout keeps the existing core.

`results.json` records executable and ELF SHA256 identities, build settings,
checks, mapping measurements, and observed console intervals. Zero FPS entries
mean no new presented frames on the loading map; they are not gameplay scores.
Game bytes, derived analysis images/decompilation, console screenshots, logs,
keys, firmware, SDKs, and binary packages are excluded.

To reconstruct on a separate clean checkout of this PR:

```sh
git apply --index experiments/selevet-mkw/ps5-port.patch
git submodule sync -- Eglantine
git submodule update --init -- Eglantine
bash tools/apply-eglantine-patches.sh
```

The root snapshot records the Selevet gitlink and introduces the maintained
`tools/eglantine-patches/0001-selevet-ps5-port.patch`. Both patches preserve
upstream line endings. Do not run an autoformatter on patch contents.

The snapshot includes captured pre-existing PS5/MKW compatibility work from
the original checkout, plus the fork adaptation. It does not establish that
all behavior differences from another session belong to Selevet. CPU tuning
comparisons require identical baseline source, renderer, memory policy,
settings, save and rendered scene. See the report for what was actually tested.
