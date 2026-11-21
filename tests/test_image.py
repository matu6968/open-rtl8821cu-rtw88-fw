# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src' / 'diagnostic'))
from pack import Elf32, parse_header, split_sections, verify

IMAGE = Path(os.environ.get('FIRMWARE_IMAGE', ROOT / 'target_firmware' / 'rtw8821c_fw.bin'))
ELF = Path(os.environ.get('FIRMWARE_ELF', ROOT / 'build' / 'firmware' / 'rtw8821c_fw.elf'))


class ProductionImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = IMAGE.read_bytes()
        cls.elf = Elf32(ELF.read_bytes())
        cls.manifest = json.loads(IMAGE.with_suffix('.json').read_text())

    def test_release_header_and_manifest(self):
        header = parse_header(self.image)
        self.assertEqual(IMAGE.name, 'rtw8821c_fw.bin')
        self.assertEqual((header['version'], header['subversion'], header['subindex']), (0, 11, 1))
        self.assertEqual(header['signature'], 0x8821)
        self.assertEqual((header['file_size'], header['expected_size']), (139472, 139472))
        self.assertEqual(self.manifest['image_sha256'], hashlib.sha256(self.image).hexdigest())
        self.assertEqual(self.manifest['elf_sha256'], hashlib.sha256(self.elf.data).hexdigest())
        self.assertTrue(self.manifest['instruction_audit']['reserve_t9'])
        self.assertEqual(self.manifest['layout'], self.elf.layout())

    def test_composition_and_entry_point(self):
        verify(self.image, self.elf)
        sections = split_sections(self.image, parse_header(self.image))
        self.assertEqual(struct.unpack_from('<I', sections['dmem'], 0x17c)[0], 0x80030001)
        self.assertFalse(any(sections['emem']))
        self.assertEqual([path.name for path in IMAGE.parent.glob('*.bin')], [IMAGE.name])

    def test_corrupt_section_fails_verification(self):
        corrupt = bytearray(self.image)
        corrupt[64] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum/composition mismatch'):
            verify(corrupt, self.elf)
