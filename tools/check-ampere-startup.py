#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native shader modes observed during Bananza startup."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='eden-ampere-') as directory:
    executable = Path(directory) / 'ampere-check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-I' + str(root / 'Eglantine/src'),
                    str(root / 'headless/ampere_startup_check.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Ampere startup: uniform RCP, byte LDC, F2I ceil/byte saturation, geometry emit/cut, extended uniform vote, half comparison, geometry exit cleanup and rejection controls PASS')
