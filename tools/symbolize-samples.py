#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Names the samples of headless/hang_stacks.h's profiler (EDEN_PS5_SAMPLE).

  python3 -I tools/symbolize-samples.py SAMPLES_BIN ELF [--top N] [--from S] [--to S]

Per thread: the functions the thread was in (self) and the ones on its stack (total), by share of
samples. --gap MS reports instead the stacks sampled while the thread's samples came later than
MS apart from... no: lists the samples in windows given as --from/--to seconds since the first.
"""
import argparse, collections, importlib.util, struct, subprocess
from pathlib import Path
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('symcrash', here / 'symbolize-crash.py')
sc = importlib.util.module_from_spec(spec); spec.loader.exec_module(sc)
ap = argparse.ArgumentParser(); ap.add_argument('samples'); ap.add_argument('elf')
ap.add_argument('--top', type=int, default=40); ap.add_argument('--callers', type=int, default=3)
ap.add_argument('--gaps', type=float, default=0, help='list the gaps between samples of at least this many ms')
ap.add_argument('--groups', default='', help='comma separated name parts: each sample counts for the first part found in its nearest functions')
ap.add_argument('--depth', type=int, default=6)
ap.add_argument('--callers-of', default='', help='a name part: tally the nearest callers of the samples in that function')
ap.add_argument('--from', dest='start', type=float, default=None)
ap.add_argument('--to', dest='end', type=float, default=None)
a = ap.parse_args()
elf = Path(a.elf).read_bytes(); start, at, size = sc.sections(elf)['.text']
data = Path(a.samples).read_bytes(); pos = 0; threads = []
while pos < len(data):
    end = data.index(b'\n', pos); _, name, n, words = data[pos:end].split(); n = int(n); words = int(words)
    raw = struct.unpack_from(f'<{n * words}Q', data, end + 1); pos = end + 1 + n * words * 8
    threads.append((name.decode(), [raw[i * words:(i + 1) * words] for i in range(n)]))
first = min((s[0][0] for _, s in threads if s), default=0)
wanted = set()
for _, samples in threads:
    for s in samples:
        if s[1]: wanted.add(s[1])
        wanted |= {o - 1 for o in s[2:] if 8 <= o <= size and sc.after_call(elf[at + o - 8:at + o])}
wanted = sorted(wanted); names = {}
for i in range(0, len(wanted), 4000):
    chunk = wanted[i:i + 4000]
    out = subprocess.run([sc.tool('llvm-symbolizer', 'llvm-symbolizer-21', 'llvm-symbolizer-18'), '-e', a.elf, '-f', '-C', '--no-inlines', *[hex(start + w) for w in chunk]],
                         capture_output=True, text=True).stdout
    names.update(zip(chunk, [b.strip().split('\n')[0] for b in out.strip().split('\n\n')]))
for name, samples in threads if a.gaps else []:
    # A thread the signal could not reach was in the kernel (uninterruptible): the first sample
    # after such a gap is where it came back.
    for before, after in zip(samples, samples[1:]):
        if after[0] - before[0] >= a.gaps * 1000:
            stack = [names.get(after[1], 'outside')] + [names[o - 1] for o in after[2:] if o - 1 in names]
            last = [names.get(before[1], 'outside')] + [names[o - 1] for o in before[2:] if o - 1 in names]
            print(f'{name} +{(before[0] - first) / 1e6:7.2f} s silent {(after[0] - before[0]) / 1e3:6.0f} ms\n   last seen in: ' + ' <- '.join(f[:70] for f in last) + '\n   back in: ' + ' <- '.join(f[:70] for f in stack))
for name, samples in [] if a.gaps else threads:
    if a.start is not None: samples = [s for s in samples if a.start <= (s[0] - first) / 1e6 <= (a.end or 1e9)]
    self_, total = collections.Counter(), collections.Counter()
    for s in samples:
        callers = [names[o - 1] for o in s[2:] if o - 1 in names]
        # Outside the app (system libraries, the kernel, JIT code): named by the nearest callers.
        top = names[s[1]] if s[1] in names else 'outside <- ' + ' <- '.join(f[:60] for f in callers[:a.callers])
        self_[top] += 1
        seen = {top, *callers}
        for f in seen: total[f] += 1
    if a.callers_of:
        tally = collections.Counter(); hits = 0
        for s in samples:
            if s[1] in names and a.callers_of in names[s[1]]:
                hits += 1
                chain = [names[o - 1] for o in s[2:] if o - 1 in names and a.callers_of not in names[o - 1]]
                tally[' <- '.join(f[:70] for f in chain[:2])] += 1
        print(f'== {name}: {hits} of {len(samples)} samples in {a.callers_of}')
        [print(f'   {100 * c / max(len(samples), 1):5.2f}% {f}') for f, c in tally.most_common(a.top)]
        continue
    if a.groups:
        parts = a.groups.split(',')
        tally = collections.Counter()
        for s in samples:
            chain = ([names[s[1]]] if s[1] in names else ['outside']) + [names[o - 1] for o in s[2:] if o - 1 in names]
            hit = next((part for f in chain[:a.depth] for part in parts if part in f), None)
            tally[hit or ('? ' + chain[0][:60])] += 1
        print(f'== {name}: {len(samples)} samples')
        [print(f'   {100 * c / max(len(samples), 1):5.1f}% {f}') for f, c in tally.most_common(a.top)]
        continue
    n = max(len(samples), 1)
    span = (samples[-1][0] - samples[0][0]) / 1e6 if samples else 0
    print(f'== {name}: {len(samples)} samples over {span:.1f} s')
    print('  self:'); [print(f'   {100 * c / n:5.1f}% {f[:230]}') for f, c in self_.most_common(a.top)]
    print('  on the stack:'); [print(f'   {100 * c / n:5.1f}% {f[:150]}') for f, c in total.most_common(a.top)]
