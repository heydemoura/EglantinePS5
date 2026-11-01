#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise every component of the actual generic-attribute range."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
settings = (cache / 'native-local/CMakeCache.txt').read_text().splitlines()
fmt = Path(next(line.split('=', 1)[1] for line in settings
                if line.startswith('FMT_SOURCE_DIR:STATIC='))) / 'include'
assert (fmt / 'fmt/ranges.h').is_file()
with tempfile.TemporaryDirectory(prefix='eden-attributes-') as directory:
    executable = Path(directory) / 'attribute-check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-DFMT_HEADER_ONLY',
                    '-I' + str(root / 'Eglantine/src'), '-I' + str(fmt),
                    str(root / 'headless/attribute_range_check.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Shader attributes: all 128 generic components, indices and elements; reserved boundaries PASS')
