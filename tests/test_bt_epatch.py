# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from bt_epatch import check_config, pack, parse, read_image
from bt_rom1_coverage import audit, require_complete
from build_bt_rom1 import (
    COEX_VERSION, FULL_UART_VERSION, FULL_USB_VERSION, LOAD, UART_VERSION, UART_VERSION_SLOT,
    VERSION as BASELINE_VERSION, VERSION_SLOT, build,
)

STOCK_IMAGE = os.environ.get('BT_STOCK_IMAGE')
STOCK_CS_IMAGE = os.environ.get('BT_STOCK_CS_IMAGE')
ROM1_ARTIFACTS = os.environ.get('BT_ROM1_ARTIFACTS')
COVERAGE = ROOT / 'src/bt-fw/rom1-coverage.json'
VERSION = 0x12345678
CONFIG = bytes.fromhex('55 ab 23 87 04 00 d9 00 01 0f')
UART_CONFIG = bytes.fromhex(
    '55 ab 23 87 13 00 0c 00 10 02 80 92 04 50 c5 ea 19 e1 1b fd af 5f 01 a4 0b'
)


class BluetoothEpatch(unittest.TestCase):
    def setUp(self):
        self.payloads = {0: b'rom0' + bytes(4), 1: b'rom1 entry' + bytes(4)}
        self.image = pack(VERSION, self.payloads)

    def test_deterministic_pack_and_rom_selection(self):
        self.assertEqual(self.image, pack(VERSION, dict(reversed(list(self.payloads.items())))))
        parsed = parse(self.image)
        self.assertEqual(parsed.version, VERSION)
        for rom, payload in self.payloads.items():
            with self.subTest(rom=rom):
                entry = parsed.select(rom)
                self.assertEqual(entry.chip_id, rom + 1)
                self.assertEqual(entry.offset % 16, 0)
                self.assertEqual(entry.data, payload)
                self.assertEqual(parsed.download(rom, CONFIG),
                                 payload[:-4] + bytes.fromhex('78 56 34 12') + CONFIG)
        with self.assertRaisesRegex(ValueError, 'no patch for ROM revision'):
            parsed.select(2)

    def test_rejects_malformed_container(self):
        first = parse(self.image).patches[0]
        cases = (
            ('truncated epatch', 0, self.image[:14]),
            ('signatures', 0, b'RTBTCore' + self.image[8:]),
            ('project', -7, b'\x02'),
            ('length', -6, b'\x00'),
            ('extension instruction length', -8, b'\x00'),
            ('table size', 12, struct.pack('<H', 0xffff)),
            ('chip ID', 16, struct.pack('<H', 1)),
            ('patch bounds', 18, struct.pack('<H', 3)),
            ('patch bounds', 22, struct.pack('<I', 2)),
            ('patch bounds', 26, struct.pack('<I', first.offset + 4)),
            ('patch bounds', 26, struct.pack('<I', len(self.image) - 8)),
        )
        for message, offset, replacement in cases:
            with self.subTest(message=message, offset=offset):
                if offset == 0:
                    mutated = replacement
                else:
                    start = offset % len(self.image)
                    mutated = self.image[:start] + replacement + self.image[start + len(replacement):]
                with self.assertRaisesRegex(ValueError, message):
                    parse(mutated)

    def test_rejects_invalid_pack_inputs_and_config(self):
        for version, patches in ((-1, self.payloads), (VERSION, {}),
                                 (VERSION, {256: bytes(4)}), (VERSION, {0: bytes(3)})):
            with self.subTest(version=version, patches=patches):
                with self.assertRaises(ValueError):
                    pack(version, patches)
        check_config(CONFIG)
        with self.assertRaisesRegex(ValueError, 'configuration header'):
            check_config(CONFIG[:-1])
        with self.assertRaisesRegex(ValueError, 'configuration value'):
            check_config(CONFIG[:-2] + b'\x02\x0f')

    def test_controller_applies_the_board_record(self):
        for transport, config, flags in (('usb', CONFIG, ()),
                                         ('uart', UART_CONFIG, ('-DBT_ROM1_UART',))):
            with self.subTest(transport=transport), tempfile.TemporaryDirectory() as work:
                executable = Path(work) / 'bt_config_model'
                subprocess.run([
                    os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', *flags, str(ROOT / 'tests/bt_config_model.c'),
                    str(ROOT / 'src/bt-fw/config.c'), '-o', str(executable),
                ], check=True)
                subprocess.run([str(executable)], input=config, check=True)

    def test_full_config_replays_multiple_records_and_rejects_partial_writes(self):
        records = UART_CONFIG[6:] + CONFIG[6:] + bytes.fromhex('07 10 04 01 02 03 04 f3 03 01 88')
        config = CONFIG[:4] + struct.pack('<H', len(records)) + records
        check_config(config)
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'bt_rom1_config_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'src/bt-fw'), str(ROOT / 'tests/bt_rom1_config_model.c'),
                str(ROOT / 'src/bt-fw/rom1_config.c'), '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], input=config, check=True)

    def test_full_patch_reads_factory_efuse_records(self):
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'bt_rom1_efuse_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                str(ROOT / 'tests/bt_rom1_efuse_model.c'), str(ROOT / 'src/bt-fw/rom1_efuse.c'),
                '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    @unittest.skipUnless(sys.platform == 'linux', 'requires fixed-address userspace mappings')
    def test_uart_dma_replays_descriptor_reset_and_interrupts(self):
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'bt_uart_dma_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                str(ROOT / 'tests/bt_uart_dma_model.c'), str(ROOT / 'src/bt-fw/uart_dma.c'),
                '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_full_coverage_rejects_a_gap_and_reports_pending_work(self):
        for transport, end in (('usb', VERSION_SLOT), ('uart', UART_VERSION_SLOT)):
            with self.subTest(transport=transport):
                spec = json.loads(COVERAGE.read_text())[transport]
                extent = end - LOAD + 4
                totals, pending = audit(spec, LOAD, extent)
                self.assertEqual(sum(totals.values()), extent)
                self.assertGreater(totals['code'], 0)
                if any(pending.values()):
                    with self.assertRaisesRegex(ValueError, 'coverage incomplete'):
                        require_complete(COVERAGE, transport, LOAD, extent, Path('missing.elf'), 'nm')
                spec['regions'].pop(0)
                with self.assertRaisesRegex(ValueError, 'invalid coverage region'):
                    audit(spec, LOAD, extent)

    def test_wlan_afh_updates_the_controller_channel_map(self):
        with tempfile.TemporaryDirectory() as work:
            executable = Path(work) / 'bt_afh_model'
            subprocess.run([
                os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                '-Werror', str(ROOT / 'tests/bt_afh_model.c'),
                str(ROOT / 'src/bt-fw/afh.c'), '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_source_rom1_artifacts_and_no_rom0_fallback(self):
        elf = bytearray(52)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHII', elf, 16, 2, 8, 1, LOAD | 1)
        patch = b'\xb2\x05' + bytes(VERSION_SLOT - LOAD + 2)
        image, config, manifest = build(bytes(elf), patch)
        self.assertEqual(config, CONFIG)
        parsed = parse(image)
        self.assertEqual(parsed.select(1).chip_id, 2)
        self.assertEqual(parsed.select(1).data, patch)
        with self.assertRaisesRegex(ValueError, 'no patch for ROM revision 0'):
            parsed.select(0)
        wire = parsed.download(1, config)
        self.assertEqual(len(wire), 34926)
        self.assertEqual(hashlib.sha256(wire).hexdigest(), manifest['download_sha256'])
        self.assertEqual(wire[-14:-10], struct.pack('<I', parsed.version))
        with self.assertRaisesRegex(ValueError, 'patch extent'):
            build(bytes(elf), patch[:-1])
        struct.pack_into('<I', elf, 24, LOAD)
        with self.assertRaisesRegex(ValueError, 'tagged MIPS16 entry'):
            build(bytes(elf), patch)

    def test_rom1_uart_layout_and_required_baud_config(self):
        elf = bytearray(52)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHII', elf, 16, 2, 8, 1, LOAD | 1)
        patch = b'\xb2\x05' + bytes(UART_VERSION_SLOT - LOAD + 2)
        image, config, manifest = build(bytes(elf), patch, transport='uart')
        self.assertEqual(config, UART_CONFIG)
        self.assertEqual(struct.unpack_from('<HBI', config, 6), (0x000c, 16, 0x04928002))
        self.assertTrue(config[9 + 12] & 4)
        self.assertEqual(manifest['status'], 'hardware-untested')
        parsed = parse(image)
        self.assertEqual(parsed.version, UART_VERSION)
        wire = parsed.download(1, config)
        self.assertEqual(len(wire), 36953)
        self.assertEqual(wire, patch[:-4] + struct.pack('<I', UART_VERSION) + config)
        with self.assertRaisesRegex(ValueError, 'patch extent'):
            build(bytes(elf), patch)
        with self.assertRaisesRegex(ValueError, 'transport and coexistence'):
            build(bytes(elf), patch, transport='uart', coex=True)

    @unittest.skipUnless(ROM1_ARTIFACTS, 'use make check-bluetooth to test the built output')
    def test_linked_rom1_download(self):
        output = Path(ROM1_ARTIFACTS)
        manifest = json.loads((output / 'manifest.json').read_text())
        uart = os.environ.get('BT_ROM1_TRANSPORT') == 'uart'
        chip = 'rtl8821cs' if uart else 'rtl8821c'
        image = (output / f'rtl_bt/{chip}_fw.bin').read_bytes()
        config = (output / f'rtl_bt/{chip}_config.bin').read_bytes()
        parsed = parse(image)
        coex = os.environ.get('BT_ROM1_COEX') == '1'
        full = os.environ.get('BT_ROM1_PROFILE') == 'full'
        version = UART_VERSION if uart else (COEX_VERSION if coex else BASELINE_VERSION)
        if full:
            version = FULL_UART_VERSION if uart else FULL_USB_VERSION
            self.assertEqual(manifest['profile'], 'full')
            self.assertEqual(manifest['coverage']['inventory_sha256'],
                             hashlib.sha256(COVERAGE.read_bytes()).hexdigest())
        version_slot = UART_VERSION_SLOT if uart else VERSION_SLOT
        expected_config = UART_CONFIG if uart else CONFIG

        self.assertEqual(manifest['rom_revision'], 1)
        if uart:
            self.assertEqual(manifest['controller'], 'rtl8821cs')
            self.assertEqual(manifest['transport'], 'uart-h5')
            self.assertEqual(manifest['status'], 'hardware-untested')
            self.assertEqual(manifest['uart_baud'], 1500000)
            self.assertTrue(manifest['uart_rtscts'])
        else:
            self.assertEqual(manifest['board_profile'], '0x04222141')
        self.assertIs(manifest['coexistence_probe'], coex and not full)
        self.assertEqual(manifest['version_address'], f'0x{version_slot:08x}')
        self.assertEqual(manifest['firmware_version'], f'0x{version:08x}')
        self.assertEqual(parsed.version, version)
        self.assertEqual(config, expected_config)
        self.assertEqual([entry.chip_id for entry in parsed.patches], [2])
        with self.assertRaisesRegex(ValueError, 'no patch for ROM revision 0'):
            parsed.select(0)

        patch = parsed.select(1).data
        self.assertEqual(len(patch), version_slot - LOAD + 4)
        self.assertEqual(patch[-4:], bytes(4))
        self.assertTrue(any(patch[:4]))
        wire = parsed.download(1, config)
        self.assertEqual(wire, patch[:-4] + struct.pack('<I', version) + expected_config)
        self.assertEqual(manifest['download_bytes'], len(wire))
        for key, data in (('firmware', image), ('config', config), ('download', wire)):
            self.assertEqual(manifest[f'{key}_sha256'], hashlib.sha256(data).hexdigest())

    @unittest.skipUnless(STOCK_IMAGE, 'set BT_STOCK_IMAGE to check the recorded reference')
    def test_stock_reference_both_rom_entries(self):
        reference = read_image(Path(STOCK_IMAGE))
        self.assertEqual(hashlib.sha256(reference).hexdigest(),
                         'dfe7ef772d078ec09490c02190a32fcd40b3eaf3b040049d8930eb80e6d5743c')
        stock = parse(reference)
        self.assertEqual(stock.version, 0x75b8f098)
        expect = ((0x30, 0x4c14), (0x4cc0, 0x8864))
        repacked = parse(pack(stock.version, {
            rom: stock.select(rom).data for rom in range(2)
        }))
        for rom, (offset, length) in enumerate(expect):
            with self.subTest(rom=rom):
                patch = stock.select(rom)
                self.assertEqual((patch.offset, len(patch.data)), (offset, length))
                self.assertEqual(repacked.download(rom, CONFIG), stock.download(rom, CONFIG))
                self.assertEqual(stock.download(rom, CONFIG)[length - 4:length],
                                 bytes.fromhex('98 f0 b8 75'))

    @unittest.skipUnless(STOCK_IMAGE and STOCK_CS_IMAGE, 'set both stock images to check inventory digests')
    def test_coverage_matches_stock_rom1_payloads(self):
        specs = json.loads(COVERAGE.read_text())
        for transport, path in (('usb', STOCK_IMAGE), ('uart', STOCK_CS_IMAGE)):
            with self.subTest(transport=transport):
                payload = parse(read_image(Path(path))).select(1).data
                self.assertEqual(hashlib.sha256(payload).hexdigest(), specs[transport]['stock_sha256'])
