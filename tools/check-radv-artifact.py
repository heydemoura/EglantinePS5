#!/usr/bin/env python3
"""Exercise the actual RADV isolation/validation CLI, including stale-driver rejection."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mesa = '7b59ef27c1b09b9671bc4153c41940c3155c3af2'
sdk = 'b83202be73e930050e00de0f1b4d9ec46c0391bf'
with tempfile.TemporaryDirectory(prefix='radv-receipt-') as name:
    temp = Path(name)
    port = temp/'port'
    (port/'tools').mkdir(parents=True)
    shutil.copy2(root/'tools/isolate-radv.py', port/'tools/isolate-radv.py')
    source = temp/'mihawk-vulkan-review/.deps/native/radv-release'
    (source/'lib').mkdir(parents=True)
    (port/'tools/deps.json').write_text(json.dumps({'items': [
        {'name': 'ps5-mesa', 'commit': mesa},
        {'name': 'ps5-payload-sdk-fork', 'commit': sdk}]}))
    code = temp/'driver.c'
    code.write_text('''int mesa_shared_global;
void *radv_GetInstanceProcAddr(void) { return &mesa_shared_global; }
int cnd_init(void *p) { (void)p; return 11; }
int cnd_broadcast(void *p) { (void)p; return 12; }
int cnd_timedwait(void *p) { (void)p; return 13; }
int u_cnd_monotonic_init(void *p) { (void)p; return 21; }
int u_cnd_monotonic_broadcast(void *p) { (void)p; return 22; }
int u_cnd_monotonic_timedwait(void *p) { (void)p; return 23; }
int unrelated_condition(void *p) { return cnd_init(p) + cnd_timedwait(p); }
''')
    fixtures = {
        'driver.o': code.read_text(),
        'winsys_ps5_radv_ps5_winsys.c.o': '''extern int cnd_init(void *);
int queue_init(void *p) { return cnd_init(p); }''',
        'winsys_ps5_radv_ps5_sync.c.o': '''extern int cnd_broadcast(void *), cnd_timedwait(void *);
int queue_broadcast(void *p) { return cnd_broadcast(p); }
int queue_wait(void *p) { return cnd_timedwait(p); }''',
    }
    objects = []
    for name, body in fixtures.items():
        obj = temp/name
        subprocess.run(['cc', '-x', 'c', '-c', '-', '-o', str(obj)],
                       input=body, text=True, check=True)
        objects.append(str(obj))
    archive = source/'lib/libvulkan_radeon.ps5.a'
    subprocess.run(['ar', 'rcs', str(archive), *objects], check=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    provenance = source/'PROVENANCE.txt'
    provenance.write_text(f'revision: {mesa}\nsdk: {sdk}\nassertions: off\narchive sha256: {digest}\n')
    command = ['python3', str(port/'tools/isolate-radv.py')]
    def invoke(*options, passes=True):
        result = subprocess.run(command+list(options), capture_output=True, text=True)
        assert (result.returncode == 0) == passes, result.stdout+result.stderr
    invoke('--check', passes=False)  # A missing archive is never a verified artifact.
    invoke()
    invoke('--check')
    isolated = port/'build/radv-isolated/libvulkan_radeon.ps5.a'
    symbols = subprocess.check_output(['nm', '-g', str(isolated)], text=True)
    assert ' eden_radv_private_mesa_shared_global' in symbols
    assert ' radv_GetInstanceProcAddr' in symbols
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest
    harness = temp/'behavior.c'
    harness.write_text('''#include <assert.h>
extern int eden_radv_private_queue_init(void *), eden_radv_private_queue_broadcast(void *);
extern int eden_radv_private_queue_wait(void *), eden_radv_private_unrelated_condition(void *);
int main(void) {
    assert(eden_radv_private_queue_init(0) == 21);
    assert(eden_radv_private_queue_broadcast(0) == 22);
    assert(eden_radv_private_queue_wait(0) == 23);
    assert(eden_radv_private_unrelated_condition(0) == 24);
}
''')
    subprocess.run(['cc', str(harness), str(isolated), '-o', str(temp/'behavior')], check=True)
    subprocess.run([str(temp/'behavior')], check=True)
    manifest = port/'build/radv-isolated/manifest.json'
    receipt = manifest.read_bytes()
    for key, value in [('source', str(temp/'diagnostic-driver.a')),
                       ('source_sha256', '0'*64), ('isolated_sha256', '0'*64),
                       ('release_provenance_sha256', '0'*64),
                       ('mesa_revision', 'other-mesa'), ('sdk_revision', 'other-sdk'),
                       ('sync_clock_adapter', 'old-or-missing-adapter'),
                       ('sync_clock_adapter_sha256', '0'*64)]:
        fields = json.loads(receipt)
        fields[key] = value
        manifest.write_text(json.dumps(fields))
        invoke('--check', passes=False)
    manifest.write_bytes(receipt)
    saved = isolated.read_bytes()
    isolated.write_bytes(saved+b'changed')
    invoke('--check', passes=False)
    isolated.write_bytes(saved)
    original = provenance.read_text()
    for old, new in [(mesa, 'other-mesa'), (sdk, 'other-sdk'),
                     ('assertions: off', 'assertions: on'), (digest, '0'*64)]:
        provenance.write_text(original.replace(old, new))
        invoke('--check', passes=False)
        invoke(passes=False)
    provenance.write_text(original)
    invoke('--check')
    saved_source = archive.read_bytes()
    subprocess.run(['ar', 'd', str(archive), 'winsys_ps5_radv_ps5_sync.c.o'], check=True)
    changed_digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    provenance.write_text(original.replace(digest, changed_digest))
    invoke(passes=False)  # A changed driver structure must never silently omit the adapter.
    archive.write_bytes(saved_source)
    provenance.write_text(original)
    invoke('--check')
    deps = port/'tools/deps.json'
    saved_deps = deps.read_bytes()
    for pin, replacement in [(mesa, 'new-mesa'), (sdk, 'new-sdk')]:
        deps.write_text(saved_deps.decode().replace(pin, replacement))
        provenance.write_text(original.replace(pin, replacement))
        invoke(passes=False)  # Updating both receipts still requires reviewing the clock ABI.
    deps.write_bytes(saved_deps)
    provenance.write_text(original)
    invoke('--check')
print('RADV artifact PASS: namespace isolation, source preservation, scoped monotonic condition calls, pinned release/SDK and stale/tampered artifact rejection')
