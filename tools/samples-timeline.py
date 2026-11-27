#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A timeline of the profiler's samples (headless/hang_stacks.h, EDEN_PS5_SAMPLE).

  python3 -I tools/samples-timeline.py SAMPLES_BIN ELF --from S --to S [--step MS] [--threads A,B]

Per step and thread: the most frequent nearest app functions of the samples in it ('-' = no
sample: the thread was not reached, usually blocked in the kernel on a lock)."""
import argparse, collections, importlib.util, struct, subprocess
from pathlib import Path
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('symcrash', here / 'symbolize-crash.py')
sc = importlib.util.module_from_spec(spec); spec.loader.exec_module(sc)
ap = argparse.ArgumentParser(); ap.add_argument('samples'); ap.add_argument('elf')
ap.add_argument('--from', dest='start', type=float, required=True); ap.add_argument('--to', dest='end', type=float, required=True)
ap.add_argument('--step', type=float, default=50); ap.add_argument('--threads', default=''); ap.add_argument('--depth', type=int, default=3)
a = ap.parse_args()
elf = Path(a.elf).read_bytes(); start, at, size = sc.sections(elf)['.text']
data = Path(a.samples).read_bytes(); pos = 0; threads = []
while pos < len(data):
    end = data.index(b'\n', pos); _, name, n, words = data[pos:end].split(); n = int(n); words = int(words)
    raw = struct.unpack_from(f'<{n * words}Q', data, end + 1); pos = end + 1 + n * words * 8
    threads.append((name.decode(), [raw[i * words:(i + 1) * words] for i in range(n)]))
first = min((s[0][0] for _, s in threads if s), default=0)
only = [t for t in a.threads.split(',') if t]
threads = [(n, [s for s in ss if a.start <= (s[0] - first) / 1e6 <= a.end]) for n, ss in threads if not only or any(n.startswith(o) for o in only)]
wanted = set()
for _, ss in threads:
    for s in ss:
        if s[1]: wanted.add(s[1])
        wanted |= {o - 1 for o in s[2:] if 8 <= o <= size and sc.after_call(elf[at + o - 8:at + o])}
wanted = sorted(wanted); names = {}
for i in range(0, len(wanted), 4000):
    chunk = wanted[i:i + 4000]
    out = subprocess.run([sc.tool('llvm-symbolizer', 'llvm-symbolizer-21', 'llvm-symbolizer-18'), '-e', a.elf, '-f', '-C', '--no-inlines', *[hex(start + w) for w in chunk]], capture_output=True, text=True).stdout
    names.update(zip(chunk, [b.strip().split('\n')[0] for b in out.strip().split('\n\n')]))
skip = ('__emutls', 'std::__1::', 'eden_heap', '__wrap_', 'VideoCommon::Nx2Profile', 'thread_start', 'void* std')
def short(f): return f.split('(')[0].replace('VideoCommon::', '').replace('Vulkan::', '').replace('Tegra::', '')[-46:]
def label(s):
    chain = ([names[s[1]]] if s[1] in names else []) + [names[o - 1] for o in s[2:] if o - 1 in names]
    chain = [short(f) for f in chain if not f.startswith(skip)]
    return '<'.join(chain[:a.depth]) or 'outside'
steps = int((a.end - a.start) * 1000 / a.step) + 1
for name, ss in threads:
    print(f'== {name}')
    buckets = collections.defaultdict(collections.Counter)
    for s in ss: buckets[int(((s[0] - first) / 1e6 - a.start) * 1000 / a.step)][label(s)] += 1
    last = None
    for b in range(steps):
        text = '-' if not buckets[b] else '; '.join(f'{f} x{c}' for f, c in buckets[b].most_common(2))
        if text != last: print(f'  +{a.start + b * a.step / 1000:7.2f} {text[:230]}')
        last = text
