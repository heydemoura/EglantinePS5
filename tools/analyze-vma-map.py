#!/usr/bin/env python3
"""Separate empty VMA blocks from free ranges sharing blocks with live allocations.

An unused range in a live block is not immediately reclaimable device memory.
Snapshots can change between allocator statistics and detailed-map collection.
"""
import argparse
import json
from pathlib import Path


def analyze(data):
    pools = []
    def visit(value, path):
        if isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, path + '/' + str(index))
        elif isinstance(value, dict):
            if 'Blocks' in value:
                pools.append((path, value))
            else:
                for key, child in value.items():
                    visit(child, path + '/' + key)
    for key in ('DefaultPools', 'CustomPools'):
        visit(data.get(key, {}), key)
    if not pools:
        raise ValueError('No detailed VMA pools found; request a detailed-map snapshot')
    blocks = []
    dedicated_bytes = 0
    for path, pool in pools:
        for key, block in pool['Blocks'].items():
            total, unused, count = (int(block[k]) for k in ('TotalBytes', 'UnusedBytes', 'Allocations'))
            assert 0 <= unused <= total
            ranges = block['Suballocations']
            free = [r for r in ranges if r.get('Type') == 'FREE']
            assert sum(int(r['Size']) for r in free) == unused
            assert len([r for r in ranges if r.get('Type') != 'FREE']) == count
            assert count != 0 or unused == total
            blocks.append(dict(pool=path, block=key, bytes=total, unused_bytes=unused,
                               allocations=count, largest_unused_range=max((int(r['Size']) for r in free), default=0)))
        dedicated_bytes += sum(int(a['Size']) for a in pool.get('DedicatedAllocations', []))
    return dict(block_count=len(blocks), dedicated_bytes=dedicated_bytes,
                block_bytes=sum(b['bytes'] for b in blocks),
                empty_block_bytes=sum(b['bytes'] for b in blocks if b['allocations'] == 0),
                unused_in_live_blocks=sum(b['unused_bytes'] for b in blocks if b['allocations']),
                blocks=sorted(blocks, key=lambda b: b['unused_bytes'], reverse=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    result = analyze(json.loads(args.snapshot.read_text()))
    target = args.snapshot.with_suffix('.analysis.json')
    target.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'blocks'}, indent=2))

if __name__ == '__main__':
    main()
