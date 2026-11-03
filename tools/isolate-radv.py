#!/usr/bin/env python3
"""Keep the RADV/Mesa implementation private beside the OpenGL Mesa build.

Only defined implementation symbols are renamed, together with their references
inside the archive. Public Vulkan entrypoints and shared C++ runtime identities
remain unchanged. The input release archive is never modified.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile

# In this pair of revisions, Mesa's cnd_t is pthread_cond_t and
# u_cnd_monotonic contains that same type at offset zero. check-radv-native.py
# checks the target SDK representation. Review this adapter when either pin moves.
SYNC_CLOCK_MESA = '7b59ef27c1b09b9671bc4153c41940c3155c3af2'
SYNC_CLOCK_SDK = 'b83202be73e930050e00de0f1b4d9ec46c0391bf'
SYNC_CLOCK_ADAPTER = 'ps5-monotonic-sync-v1'


def adapt_sync_clock(source, directory):
    """Use Mesa's monotonic helpers for just the PS5 queue's sync condition.

    The pinned source passes os_time_get_nano() deadlines to a default realtime
    condition. Redirecting these references is ABI-equivalent to changing its
    condition type and the init/broadcast/timedwait calls in the source. Other
    Mesa C11 condition variables keep their original clock/semantics.
    """
    replacements = {
        'winsys_ps5_radv_ps5_winsys.c.o': {'cnd_init': 'u_cnd_monotonic_init'},
        'winsys_ps5_radv_ps5_sync.c.o': {
            'cnd_broadcast': 'u_cnd_monotonic_broadcast',
            'cnd_timedwait': 'u_cnd_monotonic_timedwait'},
    }
    members = subprocess.check_output(['ar', 't', str(source)], text=True).splitlines()
    defined = symbols(source, True)
    for mapping in replacements.values():
        if not set(mapping.values()) <= defined:
            raise ValueError('RADV lacks the expected monotonic condition helpers')
    objects = []
    for member, mapping in replacements.items():
        if members.count(member) != 1:
            raise ValueError(f'Expected exactly one RADV sync member: {member}')
        obj = directory/member
        obj.write_bytes(subprocess.check_output(['ar', 'p', str(source), member]))
        if not set(mapping) <= symbols(obj) - symbols(obj, True):
            raise ValueError(f'Pinned RADV sync references changed: {member}')
        flags = [flag for old, new in mapping.items()
                 for flag in ('--redefine-sym', f'{old}={new}')]
        subprocess.run(['objcopy', *flags, str(obj)], check=True)
        assert not set(mapping).intersection(symbols(obj))
        objects.append(obj)
    adapted = directory/'clock-fixed.a'
    shutil.copy2(source, adapted)
    subprocess.run(['ar', 'r', str(adapted), *map(str, objects)], check=True)
    return adapted

def symbols(path, defined=False):
    cmd = ['nm', '-g', '--format=posix']
    if defined:
        cmd.append('--defined-only')
    result = subprocess.run(cmd+[str(path)], check=True, capture_output=True, text=True)
    return {p[0] for line in result.stdout.splitlines()
            if len(p := line.split()) >= 2 and len(p[1]) == 1}

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Validate the existing isolated archive without modifying it')
    args = parser.parse_args()
    source = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a'
    output = root/'build/radv-isolated'
    before = digest(source)
    provenance_path = source.parent.parent/'PROVENANCE.txt'
    provenance = provenance_path.read_text()
    fields = dict(line.split(': ', 1) for line in provenance.splitlines() if ': ' in line)
    pins = {item['name']: item.get('commit') for item in
            json.loads((root/'tools/deps.json').read_text())['items']}
    for key, expected in [('archive sha256', before), ('revision', pins['ps5-mesa']),
                          ('sdk', pins['ps5-payload-sdk-fork']), ('assertions', 'off')]:
        if fields.get(key) != expected:
            raise ValueError(f'RADV release provenance mismatch: {key}; rebuild dependencies')
    if fields['revision'] != SYNC_CLOCK_MESA or fields['sdk'] != SYNC_CLOCK_SDK:
        raise ValueError('Review the RADV monotonic condition ABI adapter for the new Mesa/SDK pins')
    target = output/source.name
    if args.check:
        manifest = json.loads((output/'manifest.json').read_text())
        if Path(manifest['source']).resolve() != source.resolve():
            raise ValueError('Isolated RADV came from a different source archive')
        if manifest['source_sha256'] != before or manifest['isolated_sha256'] != digest(target):
            raise ValueError('Stale or modified isolated RADV archive; run tools/isolate-radv.py')
        if manifest.get('release_provenance_sha256') != digest(provenance_path):
            raise ValueError('Isolated RADV lacks matching release provenance; run tools/isolate-radv.py')
        if (manifest.get('mesa_revision') != fields['revision'] or
                manifest.get('sdk_revision') != fields['sdk']):
            raise ValueError('Isolated RADV revision receipt differs from release provenance')
        if (manifest.get('sync_clock_adapter') != SYNC_CLOCK_ADAPTER or
                manifest.get('sync_clock_adapter_sha256') != digest(Path(__file__))):
            raise ValueError('Isolated RADV lacks the monotonic sync adapter; run tools/isolate-radv.py')
        print('Isolated RADV matches the pinned PS5_Vulkan release, SDK and monotonic sync adapter')
        return
    output.mkdir(parents=True, exist_ok=True)
    defined = sorted(symbols(source, True))
    demangled = subprocess.run(['c++filt'], input='\n'.join(defined)+'\n',
                               check=True, capture_output=True, text=True).stdout.splitlines()
    assert len(defined) == len(demangled)
    public = {s for s in defined if re.match(r'vk[A-Z]|vk_icd', s)} | {'radv_GetInstanceProcAddr'}
    assert 'radv_GetInstanceProcAddr' in defined
    private = []
    for name, readable in zip(defined, demangled):
        # A std:: container instantiated with aco/GLSL types still belongs to that
        # compiler version. Isolate its weak methods too, or the linker may silently
        # coalesce incompatible implementations from the OpenGL compiler archive.
        shared_cpp = ((re.match(r'^(typeinfo (for|name for) std::|vtable for std::)', readable)
                       and '<' not in readable)
                      or readable.startswith(('operator new', 'operator delete'))
                      or name.startswith(('__cxa_', '__gxx_', '_Unwind_')))
        if name not in public and not shared_cpp:
            private.append(name)
    mapping = output/'symbols.map'
    mapping.write_text(''.join(f'{s} eden_radv_private_{s}\n' for s in private))
    target = output/source.name
    with tempfile.TemporaryDirectory(prefix='sync-clock-', dir=output) as name:
        adapted = adapt_sync_clock(source, Path(name))
        adapted_sha256 = digest(adapted)
        subprocess.run(['objcopy', '--redefine-syms='+str(mapping), str(adapted), str(target)], check=True)
    subprocess.run(['ranlib', str(target)], check=True)
    assert digest(source) == before, 'Input RADV archive changed'
    after_symbols = symbols(target)
    assert not set(private).intersection(after_symbols), 'Unrenamed implementation symbol'
    assert public.intersection(defined) <= symbols(target, True), 'Lost public entrypoint'
    (output/'manifest.json').write_text(json.dumps({
        'source': str(source), 'source_sha256': before,
        'release_provenance_sha256': digest(provenance_path),
        'mesa_revision': fields['revision'], 'sdk_revision': fields['sdk'],
        'sync_clock_adapter': SYNC_CLOCK_ADAPTER,
        'sync_clock_adapter_sha256': digest(Path(__file__)),
        'adapted_sha256': adapted_sha256,
        'isolated_sha256': digest(target), 'renamed_symbols': len(private),
        'public_symbols': sorted(public.intersection(defined)),
        'note': 'Pinned PS5 sync condition clock adapter and namespace isolation; final native link and runtime validation still required.'
    }, indent=2)+'\n')
    print(f'RADV isolated: {len(private)} implementation symbols; {target}')


if __name__ == '__main__':
    main()
