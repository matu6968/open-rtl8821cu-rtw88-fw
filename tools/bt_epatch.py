#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect and pack RTL8821CU Bluetooth Realtech epatch v1 containers."""

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import subprocess

SIGNATURE = b'Realtech'
EXTENSION_SIGNATURE = bytes.fromhex('51 04 fd 77')
PROJECT = 10
HEADER = struct.Struct('<8sIH')
VERSION = struct.Struct('<I')
CONFIG_HEADER = struct.Struct('<IH')
CONFIG_MAGIC = 0x8723AB55


@dataclass(frozen=True)
class Patch:
    chip_id: int
    offset: int
    data: bytes


@dataclass(frozen=True)
class Epatch:
    version: int
    patches: tuple[Patch, ...]

    def select(self, rom: int) -> Patch:
        for patch in self.patches:
            if patch.chip_id == rom + 1:
                return patch
        raise ValueError(f'no patch for ROM revision {rom}')

    def download(self, rom: int, config: bytes = b'') -> bytes:
        if config:
            check_config(config)
        patch = self.select(rom)
        return patch.data[:-VERSION.size] + VERSION.pack(self.version) + config


def parse(data: bytes) -> Epatch:
    if len(data) < HEADER.size + 8 + 4 + len(EXTENSION_SIGNATURE):
        raise ValueError('truncated epatch')
    signature, version, count = HEADER.unpack_from(data)
    if signature != SIGNATURE or not data.endswith(EXTENSION_SIGNATURE):
        raise ValueError('invalid epatch v1 signatures')
    table_end = HEADER.size + 8 * count
    if not count or table_end > len(data) - len(EXTENSION_SIGNATURE) - 4:
        raise ValueError('invalid patch table size')

    cursor = len(data) - len(EXTENSION_SIGNATURE)
    project = None
    while cursor > table_end:
        opcode = data[cursor - 1]
        if opcode == 0xff:
            break
        if cursor - 2 < table_end:
            raise ValueError('truncated extension instruction')
        length = data[cursor - 2]
        start = cursor - 2 - length
        if not length or start < table_end:
            raise ValueError('invalid extension instruction length')
        if opcode == 0:
            if length != 1 or project is not None:
                raise ValueError('invalid project instruction')
            project = data[start]
        cursor = start
    else:
        raise ValueError('missing extension terminator')
    if project != PROJECT:
        raise ValueError(f'expected RTL8821C project {PROJECT}, found {project}')

    extension_start = cursor - 1
    ids = struct.unpack_from(f'<{count}H', data, HEADER.size)
    lengths = struct.unpack_from(f'<{count}H', data, HEADER.size + count * 2)
    offsets = struct.unpack_from(f'<{count}I', data, HEADER.size + count * 4)
    if len(set(ids)) != count or any(not 1 <= chip_id <= 256 for chip_id in ids):
        raise ValueError('invalid or duplicate chip ID')
    end = table_end
    for offset, length in sorted(zip(offsets, lengths)):
        if length < VERSION.size or offset < end or offset + length > extension_start:
            raise ValueError('invalid or overlapping patch bounds')
        end = offset + length
    return Epatch(version, tuple(
        Patch(chip_id, offset, data[offset:offset + length])
        for chip_id, length, offset in zip(ids, lengths, offsets)
    ))


def pack(version: int, patches: dict[int, bytes]) -> bytes:
    """Wrap complete raw patches keyed by zero-based Bluetooth ROM revision."""
    if not 0 <= version <= 0xffffffff:
        raise ValueError('version must fit in 32 bits')
    if not patches:
        raise ValueError('at least one ROM patch is required')
    roms = sorted(patches)
    if any(not 0 <= rom <= 255 for rom in roms):
        raise ValueError('ROM revision must fit in a byte')
    lengths = [len(patches[rom]) for rom in roms]
    if any(not VERSION.size <= length <= 0xffff for length in lengths):
        raise ValueError('patch length must include the four-byte version slot and fit in 16 bits')

    count = len(roms)
    table_end = HEADER.size + 8 * count
    offsets = []
    payload = bytearray()
    for rom in roms:
        position = table_end + len(payload)
        payload.extend(bytes(-position % 16))
        offsets.append(table_end + len(payload))
        payload.extend(patches[rom])
    return b''.join((
        HEADER.pack(SIGNATURE, version, count),
        struct.pack(f'<{count}H', *(rom + 1 for rom in roms)),
        struct.pack(f'<{count}H', *lengths),
        struct.pack(f'<{count}I', *offsets),
        payload,
        bytes((0xff, PROJECT, 1, 0)),
        EXTENSION_SIGNATURE,
    ))


def check_config(data: bytes) -> None:
    if len(data) < CONFIG_HEADER.size:
        raise ValueError('truncated Bluetooth configuration')
    magic, length = CONFIG_HEADER.unpack_from(data)
    if magic != CONFIG_MAGIC or length != len(data) - CONFIG_HEADER.size:
        raise ValueError('invalid Bluetooth configuration header')
    cursor = CONFIG_HEADER.size
    while cursor < len(data):
        if cursor + 3 > len(data):
            raise ValueError('truncated Bluetooth configuration record')
        cursor += 3 + data[cursor + 2]
        if cursor > len(data):
            raise ValueError('truncated Bluetooth configuration value')


def read_image(path: Path) -> bytes:
    if path.suffix == '.zst':
        return subprocess.check_output(['zstd', '-dc', '--', str(path)])
    return path.read_bytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect')
    inspect.add_argument('image', type=Path)
    extract = commands.add_parser('extract')
    extract.add_argument('image', type=Path)
    extract.add_argument('--rom', required=True, type=lambda value: int(value, 0))
    extract.add_argument('--download', action='store_true',
                         help='emit the bytes Linux sends, including its version substitution')
    extract.add_argument('--config', type=Path, help='append board configuration in download mode')
    extract.add_argument('-o', '--output', required=True, type=Path)
    build = commands.add_parser('pack')
    build.add_argument('--version', required=True, type=lambda value: int(value, 0))
    build.add_argument('--patch', action='append', required=True, metavar='ROM=FILE')
    build.add_argument('-o', '--output', required=True, type=Path)
    args = parser.parse_args()

    try:
        if args.command == 'pack':
            patches = {}
            for entry in args.patch:
                rom, separator, path = entry.partition('=')
                if not separator or not path:
                    raise ValueError('--patch requires ROM=FILE')
                rom_id = int(rom, 0)
                if rom_id in patches:
                    raise ValueError(f'duplicate ROM revision {rom_id}')
                patches[rom_id] = Path(path).read_bytes()
            args.output.write_bytes(pack(args.version, patches))
            return

        data = read_image(args.image)
        image = parse(data)
        if args.command == 'inspect':
            print(json.dumps({
                'sha256': hashlib.sha256(data).hexdigest(),
                'size': len(data),
                'project_id': PROJECT,
                'version': f'0x{image.version:08x}',
                'patches': [{
                    'rom': item.chip_id - 1,
                    'chip_id': item.chip_id,
                    'offset': item.offset,
                    'length': len(item.data),
                    'sha256': hashlib.sha256(item.data).hexdigest(),
                } for item in image.patches],
            }, indent=2))
        else:
            if args.config and not args.download:
                raise ValueError('--config requires --download')
            payload = image.select(args.rom).data
            if args.download:
                config = read_image(args.config) if args.config else b''
                if args.config:
                    check_config(config)
                payload = image.download(args.rom, config)
            args.output.write_bytes(payload)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
