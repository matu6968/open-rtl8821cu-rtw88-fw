# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AccessPoint(unittest.TestCase):
    def test_h2c_dispatch_and_drain_station_null_on_role_switch(self):
        src = ROOT / 'src'
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'ap_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                '-Werror', '-DDIAG_HOST_TEST', '-I', str(src),
                str(ROOT / 'tests/ap_model.c'),
                *(str(src / name) for name in (
                    'ap/ap.c', 'broader/broader.c', 'powersave/lps.c', 'station/power.c',
                )),
                '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
