#!/usr/bin/env python3
"""Mario Kart World title-screen experiment: deploy a candidate package, launch the game with
per-title performance settings, measure the title screen, profile with ps5-perf, restore the
console's files. See docs/MKW-TITLE-FPS.md.

usage (under tools/console-lease.py run --wait --):
  MKW_CANDIDATE=<build/NAME> python3 tools/mkw-title/run.py EXPDIR "VARIANT;VARIANT..."
VARIANT = label[:key=value,...] where keys are the port's performance switches, plus
  measure=<seconds> (default 60) and env=NAME=VALUE|NAME=VALUE (written to env.txt).
Needs: build/<candidate>/PPSA98515 (tools/package-headless-native.sh), EXPDIR/<candidate>-matched.elf
(the build's llvm-pie.elf), EXPDIR/launch.elf (launch.c built against the payload SDK), the
ps5-perf host tool (MKW_PS5PERF) and the console test skill's console.py for captures (MKW_CAPTURE).
"""
import sys, time, json, re, hashlib, io, ftplib, subprocess, socket, os
from pathlib import Path
sys.path.insert(0, os.environ.get('MKW_FTP_TOOLS', '/var/home/bazzite/Projects/EglantinePS5/.local/bananza-title'))
from ftp_tools import get, connect, control, installer  # the session's FTP/control helpers

HOST = os.environ.get('PS5_HOST', '192.168.0.190')
MKW = '0400C3F00006E000'
REMOTE = '/data/homebrew/PPSA98515/'
CONFIG = '/data/eglantineps5/config/eglantineps5.json'
PERF = os.environ.get('MKW_PS5PERF', '/var/home/bazzite/Projects/ps5-perf/host/ps5perf.py')
CAPTURE = os.environ.get('MKW_CAPTURE', os.path.expanduser('~/.codex/skills/eglantine-ps5-console-test/scripts/console.py'))
root = Path(__file__).resolve().parents[2]
exp = Path(sys.argv[1]).resolve()
candidate = os.environ.get('MKW_CANDIDATE', 'cand1')
app = root / 'build' / candidate / 'PPSA98515'
matched_elf = exp / (candidate + '-matched.elf')
if not matched_elf.exists(): matched_elf = root / 'build/headless-native/llvm-pie.elf'
launch_elf = exp / 'launch.elf'
run = exp / str(time.time_ns()); run.mkdir(parents=True)
(exp / 'latest').write_text(str(run))
deadline = time.monotonic() + 3600


def digest(b): return hashlib.sha256(b).hexdigest()
def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)
    with (run / 'runner.log').open('a') as f: print(time.strftime('%H:%M:%S'), *a, file=f)
def put(path, data):
    with connect() as f:
        f.storbinary('STOR ' + path + '.tmp', io.BytesIO(data)); f.rename(path + '.tmp', path)
def read_optional(path):
    try: return get(path)
    except ftplib.error_perm as e:
        if str(e).startswith('550'): return None
        raise
def tail(path, n=300000):
    with connect() as f:
        size = f.size(path); b = bytearray()
        f.retrbinary('RETR ' + path, b.extend, rest=max(0, size - n) or None)
    return b.decode(errors='replace')
def capture(label):
    try:
        subprocess.run([sys.executable, CAPTURE, '--host', HOST, 'capture', '--out', str(run / (label + '.png'))],
                       check=True, timeout=60)
    except Exception as e: log('capture failed', label, repr(e))


def parse_variant(text):
    label, _, rest = text.partition(':')
    perf = {}; extra = {}
    for item in filter(None, rest.split(',')):
        item = item.strip()
        k, _, v = item.partition('=')
        if k in ('measure', 'env', 'res'): extra[k] = v
        else: perf[k] = v.lower() == 'true'
    return label, perf, extra


class Boot:
    def __init__(self): self.before = get('/data/eglantineps5/logs/boot-trace.txt').splitlines()[0]
    def wait(self):
        end = time.monotonic() + 180
        while True:
            data = get('/data/eglantineps5/logs/boot-trace.txt')
            if data.splitlines()[0] != self.before:
                self.line = data.splitlines()[0]
                self.pid = int(re.search(rb'pid (\d+)', self.line)[1]); return self.pid
            assert time.monotonic() < end, 'no fresh boot'; time.sleep(1)
    def alive(self):
        return get('/data/eglantineps5/logs/boot-trace.txt').splitlines()[0] == self.line


def frames_of(text):
    rows = re.findall(r'EDEN_VULKAN_FRAME frames=(\d+) seconds=([\d.]+) fps=([\d.]+) worst_ms=([\d.]+) total=(\d+)', text)
    return [dict(frames=int(a), seconds=float(b), fps=float(c), worst_ms=float(d), total=int(e)) for a, b, c, d, e in rows]


def run_variant(label, perf, extra, original_config):
    vdir = run / label; vdir.mkdir()
    log('=== variant', label, perf, extra)
    config = json.loads(original_config)
    games = config.setdefault('games', {})
    entry = games.setdefault(MKW, {})
    if perf: entry['performance'] = perf
    else: entry.pop('performance', None)
    # res=0.5x: the game's render resolution (settings_store.h kResolutionKeys).
    if 'res' in extra: entry['resolution'] = extra['res']
    else: entry.pop('resolution', None)
    put(CONFIG, json.dumps(config, indent=2).encode())
    (vdir / 'config.json').write_text(json.dumps(config, indent=2))
    with connect() as f:
        try: f.delete(REMOTE + 'env.txt')
        except ftplib.all_errors: pass
    if 'env' in extra:
        put(REMOTE + 'env.txt', (extra['env'].replace('|', '\n') + '\n').encode())
        (vdir / 'env.txt').write_text(extra['env'].replace('|', '\n') + '\n')
    control('kill PPSA98515')
    time.sleep(2)
    boot = Boot()
    with socket.create_connection((HOST, 9021), 5) as loader: loader.sendall(launch_elf.read_bytes())
    pid = boot.wait(); log('fresh pid', pid)
    (vdir / 'boot.txt').write_bytes(boot.line)
    # Title screen: the first ~800 presented frames cover loading and the intro.
    start = time.monotonic(); last_total = 0; run_variant.load_snaps = 0
    run_variant.relaunched = False
    while True:
        text = tail('/data/eglantineps5/logs/heap.log')
        rows = frames_of(text)
        total = rows[-1]['total'] if rows else 0
        if total != last_total: log('frames', total, 'fps', rows[-1]['fps'] if rows else None); last_total = total
        if total >= 800: break
        if not boot.alive():
            for name, path, n in [('stderr.log', '/data/eglantineps5/logs/stderr.log', 400000)]:
                pass
            raise AssertionError('process restarted during loading')
        if int((time.monotonic() - start) // 20) > getattr(run_variant, 'load_snaps', 0):
            run_variant.load_snaps = getattr(run_variant, 'load_snaps', 0) + 1
            for name, path, n in [('stderr.log', '/data/eglantineps5/logs/stderr.log', 400000), ('eden_log.txt', '/data/eglantineps5/user/log/eden_log.txt', 600000),
                                  ('heap.log', '/data/eglantineps5/logs/heap.log', 400000)]:
                try: (vdir / ('live-' + name)).write_text(tail(path, n))
                except Exception as e: log('snapshot', name, repr(e))
        # After any crash (ours or another session's) the app consumes its crash note and opens
        # the launcher with the notice instead of the forwarded game: kill it and launch again.
        if total == 0 and time.monotonic() - start > 45 and not getattr(run_variant, 'relaunched', False):
            err = tail('/data/eglantineps5/logs/stderr.log', 20000)
            if 'EDEN_LAUNCHER' in err and 'launch: Vulkan' not in err:
                log('launcher opened (crash notice consumed): relaunching')
                run_variant.relaunched = True
                control('kill PPSA98515'); time.sleep(4)
                boot = Boot()
                with socket.create_connection((HOST, 9021), 5) as loader: loader.sendall(launch_elf.read_bytes())
                pid = boot.wait(); log('fresh pid', pid)
                start = time.monotonic()
        assert time.monotonic() - start < 420, 'title not reached in 420 s'
        time.sleep(3)
    log('title reached after', round(time.monotonic() - start), 's')
    capture(label + '-title')
    measure = int(extra.get('measure', 60))
    windows = []; seen = set(); m_start = time.monotonic(); snapshots = 0
    def snapshot():
        # Logs are truncated when the crash handler restarts the app: keep live tails.
        for name, path, n in [('stderr.log', '/data/eglantineps5/logs/stderr.log', 400000),
                              ('eden_log.txt', '/data/eglantineps5/user/log/eden_log.txt', 600000),
                              ('heap.log', '/data/eglantineps5/logs/heap.log', 400000)]:
            try:
                text = tail(path, n); live = vdir / ('live-' + name)
                # A restart truncates the log: keep what the crashed process wrote.
                if live.exists() and len(text) < live.stat().st_size // 2:
                    live.rename(vdir / ('precrash-' + name))
                live.write_text(text)
            except Exception as e: log('snapshot', name, repr(e))
    while time.monotonic() - m_start < measure:
        for row in frames_of(tail('/data/eglantineps5/logs/heap.log')):
            if row['total'] > total and row['total'] not in seen:
                seen.add(row['total']); windows.append(row); log('window', row)
        if not boot.alive():
            log('process restarted during measurement'); break
        if int((time.monotonic() - m_start) // 12) > snapshots:
            snapshots += 1; snapshot()
        time.sleep(2.5)
    snapshot()
    # Profiles: precise (on-CPU) with call graphs, then wall-clock (off-CPU), then per-thread stats.
    sym = ['--sym', 'eboot.bin=' + str(matched_elf)]
    for name, args in [('precise', ['record', '-p', str(pid), '-g', '-F', '499', '-d', '10', '--max-stack', '48']),
                       ('offcpu', ['record', '-p', str(pid), '-g', '-F', '99', '-d', '6', '--max-stack', '48', '--off-cpu']),
                       ('stat', ['stat', '-p', str(pid), '-d', '5', '--per-thread'])]:
        with (vdir / (name + '.log')).open('w') as out:
            try:
                r = subprocess.run([sys.executable, PERF, '-H', HOST, '-P', '9077', *sym, '-o', str(vdir / name), *args],
                                   stdout=out, stderr=subprocess.STDOUT, timeout=180)
                log('perf', name, 'rc', r.returncode)
            except subprocess.TimeoutExpired:
                log('perf', name, 'TIMEOUT')
    for row in frames_of(tail('/data/eglantineps5/logs/heap.log')):
        if row['total'] > total and row['total'] not in seen: seen.add(row['total']); windows.append(row)
    capture(label + '-end')
    alive = boot.alive()
    summary = dict(label=label, perf=perf, pid=pid, alive_at_end=alive, windows=windows,
                   frames=sum(w['frames'] for w in windows), seconds=sum(w['seconds'] for w in windows))
    summary['fps'] = summary['frames'] / summary['seconds'] if summary['seconds'] else None
    summary['worst_ms'] = max((w['worst_ms'] for w in windows), default=None)
    for name, path in [('heap.log', '/data/eglantineps5/logs/heap.log'), ('stderr.log', '/data/eglantineps5/logs/stderr.log'),
                       ('eden_log.txt', '/data/eglantineps5/user/log/eden_log.txt')]:
        try: (vdir / name).write_bytes(get(path))
        except Exception as e: log('log collection', name, repr(e))
    eden = (vdir / 'eden_log.txt').read_text(errors='replace') if (vdir / 'eden_log.txt').exists() else ''
    keys = [l for l in eden.splitlines() if any(k in l for k in ('last 600 presents', 'GPU thread split', 'Performance:', 'Resolution ', 'Console mode', 'Error>', 'Critical>', 'Texture cache last 600', 'MKW_GC', 'GPU thread time', 'profile', 'last 10 s'))]
    stderr_text = (vdir / 'stderr.log').read_text(errors='replace') if (vdir / 'stderr.log').exists() else ''
    keys += [l for l in stderr_text.splitlines() if 'radv/ps5 submissions' in l][-12:]
    (vdir / 'key-lines.txt').write_text('\n'.join(keys[-200:]))
    summary['present_stats'] = [l for l in keys if 'last 600 presents' in l][-5:]
    (vdir / 'summary.json').write_text(json.dumps(summary, indent=2))
    log('RESULT', label, 'fps', summary['fps'], 'frames', summary['frames'], 'worst_ms', summary['worst_ms'], 'alive', alive)
    control('kill PPSA98515')
    return summary


def main():
    variants = [parse_variant(v) for v in sys.argv[2].split(';')]
    candidate = app / 'eboot.bin'
    assert candidate.is_file() and matched_elf.is_file()
    candidate_hash = digest(candidate.read_bytes())
    original_config = get(CONFIG)
    (run / 'original-config.json').write_bytes(original_config)
    original_settings = read_optional(REMOTE + 'dev-settings.txt')
    original_eboot = get(REMOTE + 'eboot.bin'); original_hash = digest(original_eboot)
    (run / 'original-eboot.sha256').write_text(original_hash)
    changed = {}
    results = []
    try:
        control('kill PPSA98515')
        for local in sorted(p for p in app.rglob('*') if p.is_file()):
            name = str(local.relative_to(app)); data = local.read_bytes()
            if name.startswith('ui/') or name in ('core-homebrew.nro',): continue
            old = read_optional(REMOTE + name)
            if old == data: continue
            if name == 'eboot.bin' and old is not None and digest(old) == candidate_hash: continue
            if name != 'eboot.bin' and name != 'sce_sys/param.json':
                log('skipping differing non-core file', name); continue
            changed[name] = old
            if old is not None:
                b = run / 'backup' / name; b.parent.mkdir(parents=True, exist_ok=True); b.write_bytes(old)
            with connect() as f: installer.upload_atomic(f, local, REMOTE + name)
            back = get(REMOTE + name)
            if digest(back) != digest(data):
                assert name == 'eboot.bin'
                log('eboot read back differs (decrypted SELF); trusting size', len(back), len(data))
            log('deployed', name, len(data))
        with connect() as f: f.sendcmd('SITE CHMOD 0777 ' + REMOTE + 'eboot.bin')
        (run / 'deployment.json').write_text(json.dumps(dict(candidate_sha256=candidate_hash, changed=list(changed), time_ns=time.time_ns()), indent=2))
        for label, perf, extra in variants:
            try:
                results.append(run_variant(label, perf, extra, original_config))
            except Exception as error:
                log('VARIANT FAILED', label, repr(error))
                vdir = run / label; vdir.mkdir(exist_ok=True)
                for name, path in [('heap.log', '/data/eglantineps5/logs/heap.log'), ('stderr.log', '/data/eglantineps5/logs/stderr.log'),
                                   ('eden_log.txt', '/data/eglantineps5/user/log/eden_log.txt'), ('crash-index.txt', '/data/eglantineps5/logs/crash-index.txt')]:
                    try: (vdir / name).write_bytes(get(path))
                    except Exception as e: log('log collection', name, repr(e))
                try:
                    for crash in (vdir / 'crash-index.txt').read_text().split()[-2:]:
                        (vdir / (crash + '.txt')).write_bytes(get('/data/eglantineps5/logs/' + crash + '.txt'))
                except Exception as e: log('crash collection', repr(e))
                results.append(dict(label=label, perf=perf, failed=repr(error)))
                try: control('kill PPSA98515')
                except Exception: pass
            (run / 'results.json').write_text(json.dumps(results, indent=2))
    finally:
        try: control('kill PPSA98515')
        except Exception as e: log('kill at restore', repr(e))
        put(CONFIG, original_config)
        assert get(CONFIG) == original_config
        with connect() as f:
            try: f.delete(REMOTE + 'env.txt')
            except ftplib.all_errors: pass
        if original_settings is None:
            with connect() as f:
                try: f.delete(REMOTE + 'dev-settings.txt')
                except ftplib.all_errors: pass
        else: put(REMOTE + 'dev-settings.txt', original_settings)
        if changed:
            for name, data in changed.items():
                if data is None:
                    with connect() as f:
                        try: f.delete(REMOTE + name)
                        except ftplib.all_errors: pass
                else:
                    tmp = run / 'backup' / name
                    with connect() as f: installer.upload_atomic(f, tmp, REMOTE + name)
            with connect() as f: f.sendcmd('SITE CHMOD 0777 ' + REMOTE + 'eboot.bin')
            log('restored', list(changed), 'eboot now', digest(get(REMOTE + 'eboot.bin'))[:12], 'original', original_hash[:12])
        log('RESTORED config/settings/eboot')
    for r in results: print(json.dumps({k: r.get(k) for k in ('label', 'perf', 'fps', 'frames', 'worst_ms', 'alive_at_end', 'present_stats', 'failed')}, indent=1))


if __name__ == '__main__':
    main()
