#!/usr/bin/env python3
"""Exercise source stamps and copies with real untracked header changes."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='eden-source-sync-') as temporary:
    folder=Path(temporary)
    tools=folder/'tools';tools.mkdir()
    script=tools/'sync-eden-source.sh'
    shutil.copyfile(root/'tools/sync-eden-source.sh',script)
    core=folder/'Eglantine';core.mkdir()
    (core/'CMakeLists.txt').write_text('# fixture\n')
    def git(*args):
        return subprocess.run(['git',*args],cwd=core,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    git('init','-q');git('add','CMakeLists.txt')
    git('-c','user.name=Source sync fixture','-c','user.email=fixture@example.invalid','commit','-qm','Fixture')
    destination=folder/'cache/source'
    def sync():
        subprocess.run(['bash',str(script),str(destination)],cwd=folder,check=True,stdout=subprocess.DEVNULL)
        return (destination/'.unpacked-from').read_text()
    original=sync()
    header=core/'new folder/header with spaces.h';header.parent.mkdir();header.write_text('first\n')
    first=sync();assert first!=original and (destination/header.relative_to(core)).read_text()=='first\n'
    assert sync()==first
    header.write_text('second\n');second=sync()
    assert second!=first and (destination/header.relative_to(core)).read_text()=='second\n'
    header.unlink();assert sync()==original and not(destination/header.relative_to(core)).exists()
print('Source sync PASS: real untracked headers, spaces, stable stamp, content changes and removal from a different CWD')
