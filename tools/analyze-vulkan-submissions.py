#!/usr/bin/env python3
"""Attribute bounded submission counters using the exact deployed, unstripped ELF.

Return addresses are resolved at pc-1. An in-process anchor supplies the ASLR
slide; this is caller attribution, not a full stack or whole-device GPU load.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess


def windows(text):
    # The PS5 C runtime prints %p without the Linux runtime's 0x prefix.
    anchors = set(re.findall(r'EDEN_GPU_SUBMIT_ANCHOR address=((?:0x)?[0-9a-fA-F]+) coverage=upload\+graphics', text))
    if len(anchors) != 1:
        raise ValueError('Need exactly one runtime anchor from one process')
    result = []
    current = None
    for line in text.splitlines():
        if 'EDEN_GPU_TIME wall_ms=' in line:
            current = {k: float(v) for k, v in re.findall(r'(\w+)=([0-9.]+)', line)}
            current['origins'] = []
        elif current is not None and 'EDEN_GPU_SUBMIT_ORIGIN ' in line:
            match = re.search(r'caller=((?:0x)?[0-9a-fA-F]+|\(nil\)) finish=(\d+) count=(\d+)', line)
            if not match:
                raise ValueError('Malformed caller row')
            caller, finish, count = match.groups()
            caller = hex(int(caller, 16)) if caller != '(nil)' else '0x0'
            current['origins'].append({'caller': caller, 'finish': bool(int(finish)), 'count': int(count)})
        elif current is not None and 'EDEN_GPU_SUBMIT_TOTAL ' in line:
            total = re.search(r'count=(\d+) overflow=(\d+)', line)
            if not total:
                raise ValueError('Malformed submission total')
            count, overflow = map(int, total.groups())
            if sum(r['count'] for r in current['origins']) + overflow != count:
                raise ValueError('Incomplete caller stream')
            current.update(count=count, overflow=overflow)
            result.append(current)
            current = None
    if not result:
        raise ValueError('No complete submission windows')
    return int(anchors.pop(), 16), result


def analyze(elf, log, nm='llvm-nm', addr2line='llvm-addr2line'):
    anchor, reports = windows(log.read_text(errors='replace'))
    symbols = subprocess.check_output([nm, '-n', '-C', str(elf)], text=True)
    matches = re.findall(r'^([0-9a-f]+) [tT] .*\bGpuTimeCountOrigin\(.*$', symbols, re.M)
    if len(matches) != 1:
        raise ValueError('ELF must contain exactly one GpuTimeCountOrigin symbol')
    slide = anchor - int(matches[0], 16)
    counts = collections.Counter()
    for window in reports:
        for row in window['origins']:
            counts[row['caller'], row['finish']] += row['count']
    rows = []
    for (caller, finish), count in counts.most_common():
        row = dict(caller=caller, finish=finish, count=count)
        if caller != '0x0':
            pc = int(caller, 16) - slide - 1
            if pc < 0:
                raise ValueError('Runtime anchor and ELF do not agree')
            info = subprocess.check_output([addr2line, '-f', '-C', '-e', str(elf), hex(pc)], text=True).splitlines()
            row.update(elf_pc=hex(pc), function=info[0], location=info[1])
            if info[0] == '??':
                raise ValueError('Unresolved caller: check matched ELF and process identity')
        else:
            row['function'] = 'uncaptured'
        rows.append(row)
    wall_ms = sum(w['wall_ms'] for w in reports)
    return dict(
        elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
        log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
        runtime_anchor=hex(anchor), slide=hex(slide), windows=len(reports),
        wall_ms=wall_ms, timestamp_span_ms=sum(w['busy_ms'] for w in reports),
        submissions=sum(w['count'] for w in reports),
        submissions_per_second=sum(w['count'] for w in reports) / (wall_ms / 1000),
        overflow=sum(w['overflow'] for w in reports), origins=rows,
        coverage='upload+graphics timestamp spans; excludes gaps and other work',
    )


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('log', type=Path)
    parser.add_argument('--nm', default='llvm-nm')
    parser.add_argument('--addr2line', default='llvm-addr2line')
    args = parser.parse_args()
    print(json.dumps(analyze(args.elf, args.log, args.nm, args.addr2line), indent=2))
