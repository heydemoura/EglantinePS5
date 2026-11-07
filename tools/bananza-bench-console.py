#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Donkey Kong Bananza scene benchmark on the console (warm caches, restored save).

Interleaves candidates and variants (A B A B ...). Each run restores the reference save, launches
Bananza with the one-shot launcher, replays a scripted input timeline, records the presented
frame-rate windows (EDEN_VULKAN_FRAME) and takes screenshots on a fixed cadence. Shader caches
are kept (warm) so the numbers measure rendering, not compilation. The original executable,
env/settings files and save are restored and verified at the end.

  python3 tools/bananza-bench-console.py --launcher .local/nir-console/launch-bananza.elf \
    --candidate base=BASE.bin,BASE.elf --variant 'default' --variant 'x|EDEN_FOO=1' \
    --events '55:a,70:a,85:a' --seconds 240 --shots 15 --out .local/neon/NEW_DIR

Events: SECONDS:ACTION with ACTION one of a, b, walk<ms>, back<ms>, left<ms>, right<ms>,
cam<ms> (right stick right), plus, minus, shot. Candidates must be input-enabled builds.
Evidence includes a private save/config backup: do not publish it.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

spec = importlib.util.spec_from_file_location('nir_console', Path(__file__).with_name('check-nir-console.py'))
nir = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nir)

FRAME = re.compile(r'EDEN_VULKAN_FRAME frames=(\d+) seconds=([\d.]+) fps=([\d.]+) worst_ms=([\d.]+) total=(\d+)')
BUTTONS = {'a': 0x2000, 'b': 0x4000, 'plus': 0x0008, 'minus': 0x100000}


def parse_variant(text):
    label, *items = text.split('|')
    game = {item[1:].partition('=')[0]: item[1:].partition('=')[2] for item in items if item.startswith('@')}
    return {'label': label, 'env': [item for item in items if item and not item.startswith('@')], 'game': game}


def parse_events(text):
    events = []
    for item in filter(None, text.split(',')):
        when, _, action = item.partition(':')
        events.append((float(when), action))
    return sorted(events)


class Bench(nir.Console):
    def __init__(self, args):
        first = args.candidates[0]
        args.candidate, args.elf = first['bin'], first['elf']
        super().__init__(args)
        self.token = 'bananza-bench-' + str(time.time_ns())
        self.active = first['label']

    def initialize(self):
        self.stop()
        for path in [self.app + '/' + name for name in ('env.txt', 'dev-settings.txt', 'compat-input.txt')]:
            self.backups[path] = self.get(path, True)
        config_path = self.data + '/config/eglantineps5.json'
        self.backups[config_path] = self.get(config_path)
        private = self.out / 'private-backup'
        private.mkdir()
        for index, (_, data) in enumerate(self.backups.items()):
            if data is not None:
                (private / str(index)).write_bytes(data)
        self.original_hash = nir.digest(self.get(self.app + '/eboot.bin'))
        self.original_eboot = self.app + '/eboot.bin.' + self.token
        self.rename(self.app + '/eboot.bin', self.original_eboot)
        self.deployed = True
        self.upload(self.args.candidates[0])
        config = json.loads(self.backups[config_path])
        profile = config['profiles']['current']
        save = self.data + '/user/nand/user/save/0000000000000000/' + profile + '/040074A01BF12000'
        if self.args.save_from:
            # Use a different stored save tree (for example a later scene) as the reference.
            save_source = Path(self.args.save_from)
        else:
            if not self.exists(save):
                raise RuntimeError('The Bananza save is absent')
            save_source = None
        self.download_tree(save, private / 'save')
        self.reference = save_source or (private / 'save')
        self.save_backup = save + '.' + self.token
        self.rename(save, self.save_backup)
        self.save = save
        (self.out / 'receipt.json').write_text(json.dumps({
            'original_sha256': self.original_hash, 'token': self.token, 'save': self.save,
            'save_backup': self.save_backup, 'reference_save': str(self.reference),
            'candidates': [{k: str(v) for k, v in c.items()} for c in self.args.candidates]}, indent=2))
        self.thread = nir.threading.Thread(target=self.klog, daemon=True)
        self.thread.start()
        self.log('Initialized; first candidate deployed (PT_LOAD verified)')

    def upload(self, candidate):
        temporary = self.app + '/.' + self.token + '.bin'
        self.put(temporary, candidate['bin'].read_bytes())
        view = self.get(temporary)
        if nir.load_segments(view) != nir.load_segments(candidate['elf'].read_bytes()):
            self.remove(temporary)
            raise RuntimeError(candidate['label'] + ': uploaded PT_LOAD segments differ from its ELF')
        if self.exists(self.app + '/eboot.bin'):
            self.remove(self.app + '/eboot.bin')
        self.rename(temporary, self.app + '/eboot.bin')
        self.candidate_hash = nir.digest(view)
        self.active = candidate['label']

    def deploy(self, candidate):
        if candidate['label'] != self.active:
            self.stop()
            self.upload(candidate)
            self.log('Deployed ' + candidate['label'] + ' (PT_LOAD verified)')

    def screenshot(self, name, patience=12):
        # ps5-web-stream reopens capture every few seconds while Bananza runs (1080p capture
        # fails beside the game; 720p works intermittently), so keep polling for a fresh frame.
        deadline = time.monotonic() + patience
        while time.monotonic() < deadline:
            try:
                url = f'http://{self.args.host}:8081'
                image = nir.urllib.request.urlopen(url + '/frame.jpeg', timeout=2).read()
                if image[:2] == b'\xff\xd8':
                    (self.current / (name + '.jpeg')).write_bytes(image)
                    return True
            except OSError:
                pass
            time.sleep(0.5)
        self.log('No screenshot available: ' + name)
        return False

    def act(self, action):
        if action == 'shot':
            self.screenshot(f'shot-{time.monotonic() - self.started:05.0f}s')
            return
        name = re.match(r'[a-z]+', action)[0]
        duration = int(action[len(name):] or 200)
        sticks = {'walk': (128, 0, 128, 128), 'back': (128, 255, 128, 128), 'left': (0, 128, 128, 128),
                  'right': (255, 128, 128, 128), 'cam': (128, 128, 255, 128), 'camleft': (128, 128, 0, 128)}
        if name in sticks:
            lx, ly, rx, ry = sticks[name]
            command = f'{time.time_ns()} 0 {min(duration, 3000)} {lx} {ly} {rx} {ry}\n'
        else:
            command = f'{time.time_ns()} {BUTTONS[name]} {min(duration, 3000)} 128 128 128 128\n'
        self.put(self.app + '/compat-input.txt', command.encode())
        self.log('Input ' + action)

    def run(self, candidate, variant, repetition):
        self.deploy(candidate)
        self.stop()
        label = f'{candidate["label"]}-{variant["label"]}-{repetition}'
        self.current = self.out / label
        self.current.mkdir()
        if self.exists(self.save):
            self.remove(self.save)
        self.upload_tree(self.reference, self.save)
        config = json.loads(self.backups[self.data + '/config/eglantineps5.json'])
        game = config.setdefault('games', {}).setdefault('040074A01BF12000', {})
        game.update(variant['game'])
        self.put(self.data + '/config/eglantineps5.json', json.dumps(config, indent=2).encode())
        (self.current / 'game-settings.json').write_text(json.dumps(game, indent=2))
        environment = ''.join(line + '\n' for line in variant['env'])
        if 'EDEN_LOG_FILTER=' not in environment:
            environment += 'EDEN_LOG_FILTER=*:Warning Render.Vulkan:Info\n'
        self.put(self.app + '/env.txt', environment.encode())
        (self.current / 'env.txt').write_text(environment)
        self.put(self.app + '/dev-settings.txt', b'replay=off\n')
        self.remove(self.app + '/compat-input.txt')
        before = self.get(self.data + '/logs/boot-trace.txt', True)
        before = before.splitlines()[0] if before else b''
        with nir.socket.create_connection((self.args.host, 9021), timeout=10) as client:
            client.sendall(self.args.launcher.read_bytes())
            client.shutdown(nir.socket.SHUT_WR)
        deadline = time.monotonic() + 90
        while True:
            data = self.get(self.data + '/logs/boot-trace.txt', True)
            if data and data.splitlines()[0] != before:
                boot = data.splitlines()[0]
                break
            if time.monotonic() > deadline:
                raise RuntimeError('No fresh candidate boot')
            time.sleep(1)
        self.log('Run ' + label + ': ' + boot.decode(errors='replace'))
        crashes_before = set((self.get(self.data + '/logs/crash-index.txt', True) or b'').decode(errors='replace').split())
        self.started = time.monotonic()
        events = list(self.args.events)
        next_shot, last_collect, crashed = self.args.shots, -30, None
        while (elapsed := time.monotonic() - self.started) < self.args.seconds:
            while events and elapsed >= events[0][0]:
                self.act(events.pop(0)[1])
            if self.args.shots and elapsed >= next_shot:
                self.screenshot(f'shot-{elapsed:05.0f}s')
                next_shot += self.args.shots
            if elapsed - last_collect >= 10:
                self.collect()
                current = (self.current / 'boot.txt').read_bytes().splitlines()
                if current and current[0] != boot and elapsed < 45:
                    # The launcher can re-execute itself once during start-up.
                    boot = current[0]
                    self.log('Start-up relaunch: ' + boot.decode(errors='replace'))
                elif not current or current[0] != boot:
                    crashed = round(elapsed)
                    self.log(f'{label}: application restarted/crashed at {crashed}s')
                    break
                index = (self.current / 'crash-index.txt').read_text(errors='replace').split() \
                    if (self.current / 'crash-index.txt').exists() else []
                if set(index) - crashes_before:
                    crashed = round(elapsed)
                    self.log(f'{label}: crash report {sorted(set(index) - crashes_before)} at {crashed}s')
                    break
                heap = (self.current / 'heap.log').read_text(errors='replace') if (self.current / 'heap.log').exists() else ''
                if not self.control('status').count('PPSA98515'):
                    crashed = round(elapsed)
                    self.log(f'{label}: application exited at {crashed}s')
                    break
                fps = FRAME.findall(heap)
                if fps:
                    self.log(f'{label} t={elapsed:.0f}s fps={fps[-1][2]} worst_ms={fps[-1][3]} total={fps[-1][4]}')
                last_collect = elapsed
            time.sleep(0.5)
        self.screenshot('end')
        self.collect()
        self.stop()
        self.collect()
        result = self.analyze(crashed)
        (self.current / 'result.json').write_text(json.dumps(result, indent=2))
        self.log(f'Finished {label}: ' + json.dumps({k: v for k, v in result.items() if k != 'series'}))
        return result

    def analyze(self, crashed):
        heap = (self.current / 'heap.log').read_text(errors='replace') if (self.current / 'heap.log').exists() else ''
        windows = [dict(frames=int(a), seconds=float(b), fps=float(c), worst_ms=float(d), total=int(e))
                   for a, b, c, d, e in FRAME.findall(heap)]
        # Window i covers roughly [5i, 5i+5] seconds after the renderer started presenting.
        start, end = self.args.window
        chosen = windows[start // 5:end // 5] if end else windows[start // 5:]
        result = {'crashed_at': crashed, 'windows': len(windows), 'series': [w['fps'] for w in windows]}
        if chosen:
            result.update(fps_weighted=round(sum(w['frames'] for w in chosen) / sum(w['seconds'] for w in chosen), 2),
                          fps_median=round(statistics.median(w['fps'] for w in chosen), 2),
                          fps_min=round(min(w['fps'] for w in chosen), 2),
                          worst_ms_max=round(max(w['worst_ms'] for w in chosen), 1))
        return result

    def restore(self):
        if not self.deployed:
            return
        self.stop()
        self.collect()
        if self.save:
            if self.exists(self.save):
                self.remove(self.save)
            self.rename(self.save_backup, self.save)
            for local in (self.out / 'private-backup/save').rglob('*'):
                if local.is_file():
                    remote = self.save + '/' + local.relative_to(self.out / 'private-backup/save').as_posix()
                    if self.get(remote) != local.read_bytes():
                        raise RuntimeError('Save restoration mismatch')
        for path, data in self.backups.items():
            if data is None:
                self.remove(path)
            else:
                self.put(path, data)
            if self.get(path, True) != data:
                raise RuntimeError('Settings restoration mismatch')
        if self.exists(self.app + '/eboot.bin'):
            self.remove(self.app + '/eboot.bin')
        self.rename(self.original_eboot, self.app + '/eboot.bin')
        if nir.digest(self.get(self.app + '/eboot.bin')) != self.original_hash:
            raise RuntimeError('Executable restoration mismatch')
        self.stop_log.set()
        if hasattr(self, 'thread'):
            self.thread.join(timeout=3)
        (self.out / 'restored.json').write_text(json.dumps({'executable_verified': True,
            'settings_verified': True, 'save_byte_identical': True}, indent=2))
        self.log('Original executable, settings and save restored and verified')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='192.168.0.190')
    parser.add_argument('--candidate', action='append', required=True, help='LABEL=BIN,ELF')
    parser.add_argument('--variant', action='append', default=None, help='LABEL|NAME=VALUE|...')
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=240)
    parser.add_argument('--shots', type=float, default=15, help='screenshot cadence in seconds (0: off)')
    parser.add_argument('--events', default='55:a,70:a,85:a')
    parser.add_argument('--window', default='0:0', help='START:END seconds of FPS windows to summarize')
    parser.add_argument('--save-from', default=None, help='local save tree to use as the reference')
    args = parser.parse_args()
    args.candidates = []
    for text in args.candidate:
        label, _, paths = text.partition('=')
        binary, _, elf = paths.partition(',')
        args.candidates.append({'label': label, 'bin': Path(binary), 'elf': Path(elf)})
    args.variants = [parse_variant(v) for v in (args.variant or ['default'])]
    args.events = parse_events(args.events)
    args.window = tuple(int(x) for x in args.window.split(':'))
    if os.environ.get('EGLANTINE_CONSOLE_LEASE_HOST') != args.host:
        lease = Path(__file__).with_name('console-lease.py')
        subprocess.run([sys.executable, str(lease), '--host', args.host, '--owner', 'bananza-bench',
                        'run', '--wait', '--', sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]], check=True)
        return
    bench = Bench(args)
    results = []
    try:
        bench.initialize()
        for repetition in range(1, args.runs + 1):
            for candidate in args.candidates:
                for variant in args.variants:
                    result = bench.run(candidate, variant, repetition)
                    results.append({'candidate': candidate['label'], 'variant': variant['label'],
                                    'run': repetition, **result})
                    (bench.out / 'results.json').write_text(json.dumps(results, indent=2))
    finally:
        bench.restore()


if __name__ == '__main__':
    main()
