#!/usr/bin/env python3
"""Verify source synchronization causes real Ninja rebuilds after old-mtime restores."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
script = Path(os.environ.get('EDEN_SOURCE_SYNC_SCRIPT', root/'tools/sync-eden-source.sh'))
env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null'}


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'{args}:\n{result.stdout[-4000:]}')
    return result.stdout


with tempfile.TemporaryDirectory(prefix='eden-source-sync-') as directory:
    fixture = Path(directory)
    tools = fixture/'tools'
    tools.mkdir()
    shutil.copyfile(script, tools/'sync-eden-source.sh')
    source = fixture/'Eglantine'
    source.mkdir()
    mirror = fixture/'mirror'
    build = fixture/'objects'
    (source/'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(sync_fixture C)\n'
        'add_executable(answer main.c)\n')
    main = source/'main.c'
    original = '#include <stdio.h>\n#include "value.h"\nint main(void){printf("%d\\n", VALUE + 0);}\n'
    changed = original.replace('VALUE + 0', 'VALUE + 1')
    assert len(original) == len(changed)
    main.write_text(original)
    run('git', 'init', '-q', str(source))
    run('git', 'add', 'CMakeLists.txt', 'main.c', cwd=source)
    run('git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
        'commit', '--no-gpg-sign', '-qm', 'baseline', cwd=source)
    header = source/'value.h'  # Intentionally untracked, as maintained new core files are.
    header.write_text('#define VALUE 1\n')
    sentinel = mirror/'.cache/keep'
    sentinel.parent.mkdir(parents=True)
    sentinel.write_text('owned dependency cache')

    def sync():
        return run('bash', str(tools/'sync-eden-source.sh'), str(mirror))

    def answer(expected):
        run('cmake', '-S', str(mirror), '-B', str(build), '-G', 'Ninja')
        run('cmake', '--build', str(build))
        observed = run(str(build/'answer')).strip()
        assert observed == str(expected), (expected, observed)

    sync()
    answer(1)
    # Simulate an old policy mirror with correct new bytes but timestamps older
    # than its stale object. Even a matching source stamp must migrate this cache.
    main.write_text(changed)
    os.utime(main, ns=(1_000_000_000, 1_000_000_000))
    sync()
    (mirror/'main.c').write_text(changed)
    os.utime(mirror/'main.c', ns=(1_000_000_000, 1_000_000_000))
    (mirror/'.eden-sync-policy').unlink(missing_ok=True)
    sync()
    answer(2)
    # Restoring equal-length tracked content with an old mtime must rebuild.
    main.write_text(original)
    os.utime(main, ns=(1_000_000_000, 1_000_000_000))
    sync()
    assert (mirror/'main.c').read_text() == original
    answer(1)
    # New untracked headers also participate in the content stamp and rebuild.
    header.write_text('#define VALUE 3\n')
    os.utime(header, ns=(1_000_000_000, 1_000_000_000))
    sync()
    answer(3)
    header.write_text('#define VALUE 1\n')
    os.utime(header, ns=(1_000_000_000, 1_000_000_000))
    sync()
    answer(1)
    # A no-op sync must keep useful incremental objects and mirror timestamps.
    before = {name: (mirror/name).stat().st_mtime_ns for name in ['main.c', 'value.h']}
    executable_mtime = (build/'answer').stat().st_mtime_ns
    assert not sync().strip()
    answer(1)
    assert before == {name: (mirror/name).stat().st_mtime_ns for name in before}
    assert (build/'answer').stat().st_mtime_ns == executable_mtime
    obsolete = mirror/'obsolete.c'
    obsolete.write_text('removed source')
    header.write_text('#define VALUE 4\n')
    sync()
    assert not obsolete.exists()
    assert sentinel.read_text() == 'owned dependency cache'
    answer(4)

print('Source sync PASS: actual Ninja rebuilds for legacy mirrors, same-size old-mtime '
      'tracked/untracked restores; unchanged objects, dependency cache and deletion semantics')
