#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check loose-game title IDs, including ACI0 beyond the first KiB."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="eden-npdm-") as directory:
    executable = Path(directory) / "npdm-check"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(root / "headless/npdm_title_id_check.cpp"), "-o", str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print("Loose NPDM title IDs: Switch 1/2 offsets, truncation and invalid headers PASS")
