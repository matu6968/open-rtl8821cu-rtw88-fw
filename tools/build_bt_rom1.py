#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the RTL8821C ROM 1 source-built USB or UART image."""

import argparse
import hashlib
import json
from pathlib import Path
import struct

from bt_epatch import CONFIG_MAGIC, check_config, pack, parse
from bt_rom1_coverage import require_complete

ROM = 1
LOAD = 0x8010A000
VERSION_SLOT = 0x80112860
UART_VERSION_SLOT = 0x8011303C
VERSION = 0x00010007
COEX_VERSION = 0x0001000B
UART_VERSION = 0x0001000C
FULL_USB_VERSION = 0x0001000D
FULL_UART_VERSION = 0x0001000E
BOARD_PROFILE = 0x04222141
BOARD_CONFIG_OFFSET = 0x00D9
BOARD_CONFIG_VALUE = 0x0F
UART_CONFIG_OFFSET = 0x000C
UART_BAUD_WORD = 0x04928002
UART_BAUD = 1500000


def build(elf: bytes, patch: bytes, *, coex: bool = False,
          transport: str = 'usb', coverage: dict | None = None) -> tuple[bytes, bytes, dict]:
    if transport not in ('usb', 'uart') or (transport == 'uart' and coex):
        raise ValueError('unsupported ROM 1 transport and coexistence combination')

    if (len(elf) < 52 or elf[:7] != b'\x7fELF\x01\x01\x01' or
            struct.unpack_from('<HHII', elf, 16) != (2, 8, 1, LOAD | 1)):
        raise ValueError('expected a little-endian MIPS executable with a tagged MIPS16 entry')
    version_slot = UART_VERSION_SLOT if transport == 'uart' else VERSION_SLOT
    if len(patch) != version_slot - LOAD + 4 or patch[-4:] != bytes(4) or not any(patch[:4]):
        raise ValueError('invalid ROM 1 patch extent, entry, or version slot')

    if transport == 'uart':
        # Distribution RTL8821CS record. Byte 12 enables RTS/CTS with bit 2.
        # Preserve the other controller parameters from that reference profile.
        settings = struct.pack('<I', UART_BAUD_WORD) + bytes.fromhex('50 c5 ea 19 e1 1b fd af 5f 01 a4 0b')
        record = struct.pack('<HB', UART_CONFIG_OFFSET, len(settings)) + settings
        version = UART_VERSION
        profile = {
            'status': 'hardware-untested',
            'controller': 'rtl8821cs',
            'transport': 'uart-h5',
            'uart_baud': UART_BAUD,
            'uart_rtscts': True,
        }
    else:
        # Normal btrtl configuration record captured on the USB qualification board.
        record = struct.pack('<HBB', BOARD_CONFIG_OFFSET, 1, BOARD_CONFIG_VALUE)
        version = COEX_VERSION if coex else VERSION
        profile = {'status': 'experimental', 'board_profile': f'0x{BOARD_PROFILE:08x}'}

    if coverage is not None:
        version = FULL_UART_VERSION if transport == 'uart' else FULL_USB_VERSION
        profile.update(profile='full', coverage=coverage)

    config = struct.pack('<IH', CONFIG_MAGIC, len(record)) + record
    check_config(config)
    image = pack(version, {ROM: patch})
    wire = parse(image).download(ROM, config)
    manifest = {
        **profile,
        'rom_revision': ROM,
        'load_address': f'0x{LOAD:08x}',
        'version_address': f'0x{version_slot:08x}',
        'firmware_version': f'0x{version:08x}',
        'coexistence_probe': coex and coverage is None,
        'elf_sha256': hashlib.sha256(elf).hexdigest(),
        'firmware_sha256': hashlib.sha256(image).hexdigest(),
        'config_sha256': hashlib.sha256(config).hexdigest(),
        'download_sha256': hashlib.sha256(wire).hexdigest(),
        'download_bytes': len(wire),
    }
    return image, config, manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', required=True, type=Path)
    parser.add_argument('--patch', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--transport', choices=('usb', 'uart'), default='usb')
    parser.add_argument('--coex', action='store_true', help='package the USB ROM coexistence probe')
    parser.add_argument('--coverage', type=Path, help='require complete coverage for a full image')
    parser.add_argument('--nm', help='cross-toolchain nm used to verify coverage symbols')
    args = parser.parse_args()
    if args.transport == 'uart' and args.coex:
        parser.error('--coex is available for USB')
    if bool(args.coverage) != bool(args.nm):
        parser.error('--coverage and --nm are required together')
    patch = args.patch.read_bytes()
    try:
        coverage = require_complete(args.coverage, args.transport, LOAD, len(patch),
                                    args.elf, args.nm) if args.coverage else None
        image, config, manifest = build(args.elf.read_bytes(), patch, coex=args.coex,
                                        transport=args.transport, coverage=coverage)
    except ValueError as error:
        parser.error(str(error))
    rtl_bt = args.output / 'rtl_bt'
    rtl_bt.mkdir(parents=True, exist_ok=True)
    chip = 'rtl8821cs' if args.transport == 'uart' else 'rtl8821c'
    (rtl_bt / f'{chip}_fw.bin').write_bytes(image)
    (rtl_bt / f'{chip}_config.bin').write_bytes(config)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
