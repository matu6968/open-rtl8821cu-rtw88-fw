#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep incomplete Bluetooth ROM 1 reconstructions out of the firmware output."""

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


def audit(spec: dict, load: int, patch_size: int) -> tuple[Counter, dict[str, int]]:
    if not re.fullmatch(r'[0-9a-f]{64}', spec['stock_sha256']):
        raise ValueError('coverage inventory has no stock ROM 1 digest')

    cursor = load
    totals = Counter()
    tables = set()
    for first, limit, kind in spec['regions']:
        address, end = int(first, 16), int(limit, 16)
        if (address != cursor or end <= address or end > load + patch_size or
                kind not in ('code', 'data', 'alignment', 'unclassified', 'unowned-code', 'version')):
            raise ValueError(f'invalid coverage region at 0x{address:08x}')
        if kind == 'version' and (end != load + patch_size or end - address != 4):
            raise ValueError('incorrect version region in coverage inventory')
        if kind == 'data':
            tables.add(first)
        totals[kind] += end - address
        cursor = end
    if cursor != load + patch_size or totals['version'] != 4:
        raise ValueError('coverage regions must account for the entire ROM 1 entry')

    functions = spec['functions']
    if (f'{load:08x}' not in functions or
            any(not load <= int(address, 16) < cursor - 4 or int(address, 16) % 2
                for address in functions)):
        raise ValueError('invalid function addresses in coverage inventory')
    callbacks = spec['callbacks']
    if (not callbacks or any(int(slot, 16) % 4 or not 0x80120000 <= int(slot, 16) < 0x80128000
                             or target not in functions or installer not in functions
                             for slot, (target, installer) in callbacks.items())):
        raise ValueError('invalid callback addresses in coverage inventory')

    pending = {'unclassified bytes': totals['unclassified'] + totals['unowned-code']}
    for label, reference, key in (
        ('functions', set(functions), 'function_ports'),
        ('callbacks', set(callbacks), 'callback_ports'),
        ('data regions', tables, 'data_ports'),
    ):
        ports = spec[key]
        if ports.keys() - reference:
            raise ValueError(f'{key} refers to addresses outside the inventory')
        for address, (implementation, evidence) in ports.items():
            allowed = r'[A-Za-z_][0-9A-Za-z_]*'
            if label == 'functions':
                allowed += r'|rom:800[0-6][0-9a-f]{4}'
            if not re.fullmatch(allowed, implementation) or not evidence.strip():
                raise ValueError(f'invalid {key} evidence at 0x{address}')
        pending[label] = len(reference - ports.keys())
    return totals, pending


def require_complete(path: Path, transport: str, load: int, patch_size: int,
                     elf: Path, nm: str) -> dict:
    data = path.read_bytes()
    spec = json.loads(data)[transport]
    totals, pending = audit(spec, load, patch_size)
    if any(pending.values()):
        missing = ', '.join(f'{count} {kind}' for kind, count in pending.items() if count)
        raise ValueError(f'full {transport} ROM 1 coverage incomplete ({missing}). See {path}')

    defined = {}
    for line in subprocess.check_output([nm, '-P', '--defined-only', str(elf)], text=True).splitlines():
        name, kind, address, *_ = line.split()
        if load <= (int(address, 16) & 0xffffffff) < load + patch_size - 4:
            defined[name] = kind
    for key in ('function_ports', 'callback_ports', 'data_ports'):
        for implementation, _ in spec[key].values():
            if implementation.startswith('rom:'):
                continue
            types = 'tTrR' if key == 'data_ports' else 'tT'
            if defined.get(implementation) not in tuple(types):
                raise ValueError(f'coverage symbol {implementation} is missing from the patch ELF')

    return {
        'inventory_sha256': hashlib.sha256(data).hexdigest(),
        'stock_rom1_sha256': spec['stock_sha256'],
        'stock_functions': len(spec['functions']),
        'stock_callbacks': len(spec['callbacks']),
        'stock_code_bytes': totals['code'],
    }
