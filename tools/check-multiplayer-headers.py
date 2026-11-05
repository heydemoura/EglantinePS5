#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check active Ninja objects agree on the multiplayer class declarations.

The packet and room member headers are derived (headless/multiplayer_packets.cmake): every
object must see the derived copy. Guest sockets are the Lichii core's own (sockets.h,
socket_proxy.h, the BSD service): every object must see the core's header, and none a stale
derived copy from an earlier configure.
"""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1])
def ninja(*args):
    return subprocess.run(['ninja', '-C', str(build), '-t', *args],
                          capture_output=True, text=True, check=True).stdout

active = {line.split(': ', 1)[0] for line in ninja('targets', 'all').splitlines()}
derived = ('network/packet.h', 'network/room_member.h')
upstream = ('core/internal_network/sockets.h', 'core/internal_network/socket_proxy.h',
            'core/hle/service/sockets/bsd.h')
seen = dict.fromkeys(derived + upstream, 0)
wrong, target = [], None
for line in ninja('deps').splitlines():
    if line and not line.startswith(' '):
        target = line.split(': ', 1)[0]
        continue
    if target not in active:
        continue
    path = line.strip().replace('\\', '/')
    for header in derived:
        if path.endswith('/' + header):
            if path.endswith('/headless/multiplayer/' + header):
                seen[header] += 1
            else:
                wrong.append(f'{target}: {path} (expected the derived header)')
    for header in upstream:
        if path.endswith('/' + header):
            if '/headless/multiplayer/' in path:
                wrong.append(f'{target}: {path} (expected the core\'s header)')
            else:
                seen[header] += 1
missing = [header for header, count in seen.items() if not count]
if wrong or missing:
    print('\n'.join(wrong[:20]))
    sys.exit(f'Multiplayer headers: {len(wrong)} wrong-header dependencies, missing {missing}')
print(f'Multiplayer headers: {seen} PASS')
