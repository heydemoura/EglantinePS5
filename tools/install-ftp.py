#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copies a built PPSA98515 folder to a console over FTP (make install).

  tools/install-ftp.py HOST APP_DIR [--port 2121]

Needs a running FTP server on the console (for example the Payload SDK's ftpsrv). Only what
changed is sent: build/deploy-manifest.json keeps the hash and the size of each file as it was sent
to this console, and a file is left alone when its hash is the same and the console still has a
file of that name and size. param.json is always sent, and last (eboot.bin just before it).

EglantinePS5 may be open on the console: every file goes to /data/homebrew/PPSA98515 under a
hidden temporary name, the old file is removed and the new one renamed into place.
"""
import argparse
import ftplib
import hashlib
import json
import pathlib
import sys

REMOTE = '/data/homebrew/PPSA98515'
KEY = 'PPSA98515'
MANIFEST = pathlib.Path(__file__).resolve().parents[1] / 'build/deploy-manifest.json'


def connect(host, port):
    client = ftplib.FTP()
    client.connect(host, port, timeout=60)
    client.login()
    return client


def list_names(client, path):
    previous = client.pwd()
    client.cwd(path)
    try:
        return {name for name, _ in client.mlsd() if name not in {'.', '..'}}
    finally:
        client.cwd(previous)


def list_sizes(client, path):
    # name -> size of the files in a remote folder ({} when it is not there yet). The console's FTP
    # server answers MLSD only without arguments, from inside the folder.
    previous = client.pwd()
    try:
        client.cwd(path)
    except ftplib.error_perm:
        return {}
    try:
        return {name: int(facts.get('size', -1)) for name, facts in client.mlsd()
                if facts.get('type') == 'file'}
    finally:
        client.cwd(previous)


def ensure_directory(client, path):
    current = ''
    for component in path.strip('/').split('/'):
        current += f'/{component}'
        try:
            client.mkd(current)
        except ftplib.error_perm as error:
            if str(error).split(maxsplit=1)[0] != '550':
                raise


def remove_if_present(client, path):
    try:
        # ftpsrv returns 226 for a successful DELE; ftplib.FTP.delete() only permits 200/250.
        client.sendcmd(f'DELE {path}')
    except ftplib.error_perm as error:
        text = str(error).lower()
        if not (str(error).split(maxsplit=1)[0] == '550' and ('no such' in text or 'not found' in text)):
            raise


def upload_atomic(client, local, remote):
    directory = remote.rsplit('/', 1)[0]
    temporary = f"{directory}/.{remote.rsplit('/', 1)[-1]}.upload"
    ensure_directory(client, directory)
    remove_if_present(client, temporary)
    with local.open('rb') as source:
        client.storbinary(f'STOR {temporary}', source, blocksize=256 * 1024)
    remove_if_present(client, remote)
    client.rename(temporary, remote)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument('host')
    parser.add_argument('app', type=pathlib.Path)
    parser.add_argument('--port', type=int, default=2121)
    options = parser.parse_args(argv)
    host, port, app = options.host, options.port, options.app
    critical = [app / 'eboot.bin', app / 'sce_sys/param.json']
    if not all(path.is_file() for path in critical):
        sys.exit(f'{app} is not a built {KEY} folder (run make package)')
    files = sorted(p for p in app.rglob('*') if p.is_file() and p not in critical) + critical
    try:
        manifests = json.loads(MANIFEST.read_text())
    except (OSError, ValueError):
        manifests = {}
    manifest_key = f'{host}:{KEY}'
    known = manifests.get(manifest_key, {})
    sent = {}
    console = {}
    skipped = 0
    with connect(host, port) as client:
        for index, local in enumerate(files, 1):
            relative = local.relative_to(app).as_posix()
            parent = pathlib.PurePosixPath(relative).parent.as_posix()
            folder = REMOTE if parent == '.' else f'{REMOTE}/{parent}'
            if folder not in console:
                console[folder] = list_sizes(client, folder)
            size = local.stat().st_size
            digest = hashlib.sha256(local.read_bytes()).hexdigest()
            on_console = console[folder].get(local.name) == size
            same = known.get(relative) == [digest, size]
            if relative != 'sce_sys/param.json' and on_console and same:
                sent[relative] = [digest, size]
                skipped += 1
                continue
            print(f'copied [{index}/{len(files)}] {relative}', flush=True)
            upload_atomic(client, local, f'{REMOTE}/{relative}')
            sent[relative] = [digest, size]
        if skipped:
            print(f'{skipped} files were already on the console, unchanged, and were not sent again')
        manifests[manifest_key] = sent
        MANIFEST.parent.mkdir(parents=True, exist_ok=True)
        MANIFEST.write_text(json.dumps(manifests, indent=1))
        if 'eboot.bin' not in list_names(client, REMOTE):
            sys.exit('FTP upload completed but eboot.bin is not listed')
        if 'param.json' not in list_names(client, f'{REMOTE}/sce_sys'):
            sys.exit('FTP upload completed but param.json is not listed')
    print(f'Installed {len(files) - skipped} changed files in {REMOTE} on {host}.')


if __name__ == '__main__':
    main(sys.argv[1:])
