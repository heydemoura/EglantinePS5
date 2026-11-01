#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the submodule core's shader, readback and mapping regression tests on the host."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
build = root / '.local/nx2-core-check'
command = ['cmake', '-S', str(root / 'headless/nx2-core-check'), '-B', str(build),
           '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DEDEN_SOURCE=' + str(root / 'Eglantine'),
           '-DFMT_INCLUDE=' + str(cache / 'source/.cache/cpm/fmt/12.1.0/include'),
           '-DDEPENDENCY_SOURCE=' + str(cache / 'source')]
if source := os.environ.get('EDEN_CATCH2_SOURCE'):
    command.append('-DFETCHCONTENT_SOURCE_DIR_CATCH2=' + str(Path(source).resolve()))
subprocess.run(command, check=True)
subprocess.run(['cmake', '--build', str(build), '--parallel', '4'], check=True)
subprocess.run([str(build / 'nx2-core-check')], check=True, timeout=120)
