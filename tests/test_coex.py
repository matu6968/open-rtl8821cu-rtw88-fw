# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BluetoothCoexistence(unittest.TestCase):
    def test_register_mailbox_and_c2h_replay(self):
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'coex_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                '-Werror', '-DDIAG_HOST_TEST', '-DRTL8821CU_COEX_BOARD=0x04212001',
                '-I', str(ROOT / 'src'), str(ROOT / 'tests/coex_model.c'),
                str(ROOT / 'src/coex/coex.c'), '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
