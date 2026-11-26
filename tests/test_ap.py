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


class InterfacePowerPolicy(unittest.TestCase):
    def test_non_usb_power_paths_stay_awake(self):
        src = ROOT / 'src'
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'power_policy_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                '-Werror', '-DDIAG_HOST_TEST', '-I', str(src),
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                str(ROOT / 'tests/power_policy_model.c'),
                *(str(src / name) for name in (
                    'deep/deep.c', 'deep/clock.c', 'doze/beacon.c', 'doze/doze.c',
                    'doze/radio.c', 'powersave/lps.c', 'station/power.c', 'wowlan/wowlan.c',
                )),
                '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
