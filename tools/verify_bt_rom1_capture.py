#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify RTL8821CU ROM 1 firmware downloads in a cold-boot USBmon capture."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from bt_epatch import check_config, parse, read_image
from decode_fwlog import c2h_frames, pcap_records
from parse_rtw import parse_header, split_sections

USBMON_MMAPPED = 220
BT_FRAGMENT = 252
WLAN_CHUNK = 4096
WLAN_TX_DESC = 48


def expected_hci_commands(download):
    commands = []
    for number in range(len(download) // BT_FRAGMENT + 1):
        offset = number * BT_FRAGMENT
        fragment = download[offset:offset + BT_FRAGMENT]
        index = (number - 1) % 127 + 1 if number else 0
        if offset + BT_FRAGMENT > len(download):
            index |= 0x80
        commands.append(b'\x20\xfc' + bytes((len(fragment) + 1, index)) + fragment)
    return commands


def verify(path, bus, address, wlan, firmware, config):
    header = parse_header(wlan)
    if header['expected_size'] != len(wlan):
        raise ValueError('WLAN image length does not match its header')
    chunks = [section[offset:offset + WLAN_CHUNK]
              for section in split_sections(wlan, header).values()
              for offset in range(0, len(section), WLAN_CHUNK)]
    check_config(config)
    bluetooth = parse(firmware)
    expected = expected_hci_commands(bluetooth.download(1, config))
    commands = []
    chunk = 0
    reads = set()
    h2c_ext = {}
    scoreboard, h2c, coex_payloads, c2h, errors = (Counter() for _ in range(5))

    for link, _, raw, _ in pcap_records(path):
        if link != USBMON_MMAPPED or len(raw) < 64:
            raise ValueError('expected full USBmon packets with a Linux mmap header')
        event, transfer, endpoint, device, packet_bus = struct.unpack_from('<BBBBH', raw, 8)
        if (packet_bus, device) != (bus, address):
            continue
        status, length, captured = struct.unpack_from('<iII', raw, 28)
        payload = raw[64:]
        urb = raw[:8]
        if event in (ord('C'), ord('E')) and status:
            errors[str(status)] += 1

        if event == ord('S') and transfer == 2:
            kind, request, reg, _, size = struct.unpack_from('<BBHHH', raw, 40)
            if kind in (0x20, 0x21) and request == 0 and payload.startswith(b'\x20\xfc'):
                commands.append(payload)
            if kind == 0x40 and request == 5 and len(payload) == 4:
                if reg in range(0x1f0, 0x200, 4):
                    h2c_ext[reg - 0x20] = payload
                elif reg in range(0x1d0, 0x1e0, 4):
                    h2c[f'0x{payload[0]:02x}'] += 1
                    ext = h2c_ext.pop(reg, None)
                    if ext is not None and payload[0] in (0x60, 0x66, 0x69):
                        coex_payloads[(payload + ext).hex(' ')] += 1
            if kind == 0xc0 and request == 5 and reg == 0xaa and size == 2:
                reads.add(urb)
        elif event == ord('S') and transfer == 3 and not endpoint & 0x80 and chunk < len(chunks):
            expected_chunk = chunks[chunk]
            transfer_size = WLAN_TX_DESC + len(expected_chunk)
            padding = int(transfer_size % 512 == 0)
            if len(payload) == transfer_size + padding and payload[WLAN_TX_DESC:transfer_size] == expected_chunk:
                chunk += 1
        elif event in (ord('C'), ord('E')) and urb in reads:
            reads.remove(urb)
            if not status and len(payload) == 2:
                scoreboard[f'0x{int.from_bytes(payload, "little"):04x}'] += 1
        elif event == ord('C') and transfer == 3 and endpoint == 0x84 and not status:
            if payload and len(payload) == length == captured:
                try:
                    c2h.update(frame[2:].hex(' ') for frame in c2h_frames(payload)
                               if len(frame) == 9 and frame[0] == 0x09)
                except ValueError:
                    # Some USB completions contain no decodable rtw88 RX aggregate.
                    continue

    return {
        'usb_bus': bus,
        'usb_device': address,
        'wlan_sha256': hashlib.sha256(wlan).hexdigest(),
        'wlan_chunks': [chunk, len(chunks)],
        'wlan_download_matches': bool(chunks) and chunk == len(chunks),
        'bluetooth_version': f'0x{bluetooth.version:08x}',
        'bluetooth_fragments': [len(commands), len(expected)],
        'bluetooth_download_matches': commands == expected,
        'bluetooth_download_sha256': hashlib.sha256(b''.join(cmd[4:] for cmd in commands)).hexdigest(),
        'wlan_scoreboard': dict(scoreboard),
        'h2c_commands': dict(h2c),
        'h2c_coex_payloads': dict(coex_payloads),
        'c2h_bluetooth_reports': dict(c2h),
        'usb_errors': dict(errors),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--bus', type=int, required=True)
    parser.add_argument('--device', type=int, required=True)
    parser.add_argument('--wlan', type=Path, required=True)
    parser.add_argument('--bluetooth', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.bus <= 65535 or not 1 <= args.device <= 127:
        parser.error('expected a valid USB bus and device address')
    try:
        report = verify(args.capture, args.bus, args.device, read_image(args.wlan),
                        read_image(args.bluetooth), read_image(args.config))
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))
    return 0 if report['wlan_download_matches'] and report['bluetooth_download_matches'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
