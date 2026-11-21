#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode RTL8821C firmware text from a little-endian USBmon pcap."""

import argparse
import json
from pathlib import Path
import struct

CHUNK = 11
LIMIT = 55


def pcap_records(path):
    with Path(path).open('rb') as f:
        header = f.read(24)
        if len(header) != 24 or header[:4] != b'\xd4\xc3\xb2\xa1':
            raise ValueError('expected little-endian microsecond pcap')
        link = struct.unpack_from('<I', header, 20)[0]
        while h := f.read(16):
            if len(h) != 16: raise ValueError('truncated pcap record')
            sec, us, cap, original = struct.unpack('<4I', h)
            if cap > 1024*1024 or cap > original or us >= 1000000:
                raise ValueError('invalid pcap length/timestamp')
            data = f.read(cap)
            if len(data) != cap: raise ValueError('truncated pcap payload')
            yield link, sec*1000000+us, data, original


def c2h_frames(data):
    """Read C2H packets from one descriptor-aligned USB bulk completion."""
    frames = []
    offset = 0
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < 24:
            if remaining <= 7 and not any(data[offset:]):
                break
            raise ValueError('truncated RX descriptor')
        w0, _, w2, w3 = struct.unpack_from('<4I', data, offset)
        length = w0 & 0x3fff
        info = ((w0 >> 16) & 15) * 8
        shift = (w0 >> 24) & 3
        phy, c2h = bool(w0 & (1 << 26)), bool(w2 & (1 << 28))
        if not 0 < length <= 11454 or info not in (0, 32) or (phy and not info):
            raise ValueError('invalid RX length or PHY information')
        if w3 & 0x7f >= 84 or (not c2h and length <= 4):
            raise ValueError('invalid RX rate or frame length')

        begin = offset + 24 + info + shift
        end = begin + length
        if end > len(data):
            raise ValueError('RX payload exceeds completion')
        if c2h:
            frames.append(data[begin:end])
        offset = (end + 7) & ~7
    return frames


class Decoder:
    def __init__(self):
        self.packet_sequence = None
        self.message_sequence = None
        self.discarded_sequence = None
        self.remaining = 0
        self.text = bytearray()

    def discard(self, sequence=None):
        if sequence is not None:
            self.discarded_sequence = sequence
        elif self.message_sequence is not None:
            self.discarded_sequence = self.message_sequence
        self.message_sequence = None
        self.text.clear()

    def consume(self, frame):
        if len(frame) < 2:
            self.discard()
            return None
        packet_sequence = frame[1]
        gap = (self.packet_sequence is not None and
               packet_sequence != (self.packet_sequence + 1) & 255)
        self.packet_sequence = packet_sequence
        if gap:
            self.discard()
        if frame[0] != 0:
            return None
        if len(frame) < 3:
            self.discard()
            return None
        sequence, remaining = frame[2] >> 4, frame[2] & 15
        fragment = frame[3:]
        if (gap or sequence == self.discarded_sequence or remaining > LIMIT // CHUNK or
                (remaining and len(fragment) != CHUNK) or
                (not remaining and len(fragment) >= CHUNK) or
                any(not (32 <= byte < 127 or byte in (9, 10, 13)) for byte in fragment)):
            self.discard(sequence)
            return None
        if sequence != self.message_sequence:
            self.text.clear()
            self.message_sequence = sequence
            self.discarded_sequence = None
        elif self.remaining != remaining + 1:
            self.discard(sequence)
            return None
        if len(self.text) + len(fragment) > LIMIT:
            self.discard(sequence)
            return None
        self.remaining = remaining
        self.text.extend(fragment)
        if remaining:
            return None
        message = self.text.decode('ascii')
        self.message_sequence = None
        self.text.clear()
        return message


def messages(path, bus=None, device=None):
    streams = {}
    for link, timestamp, raw, _ in pcap_records(path):
        if link != 220 or len(raw) < 64:
            raise ValueError('expected USBmon 64-byte records')
        event, transfer, endpoint, address, bus_number = struct.unpack_from('<BBBBH', raw, 8)
        if (bus is not None and bus_number != bus) or (device is not None and address != device):
            continue
        key = bus_number, address
        if transfer == 2 and event == ord('S'):
            setup = raw[40:48]
            set_configuration = setup[:2] == b'\x00\x09'
            firmware_download = (setup[:6] == b'\x40\x05\x80\x00\x00\x00' and
                                 len(raw) > 64 and raw[64] & 1)
            if set_configuration or firmware_download:
                streams.pop(key, None)
        if transfer != 3 or event != ord('C') or not endpoint & 128:
            continue
        status, length, captured = struct.unpack_from('<iII', raw, 28)
        if status or not length:
            continue
        if key not in streams:
            streams[key] = Decoder()
        decoder = streams[key]
        if captured != length or len(raw) != 64 + captured:
            decoder.discard()
            continue
        try:
            frames = c2h_frames(raw[64:])
        except ValueError:
            decoder.discard()
            continue
        for frame in frames:
            message = decoder.consume(frame)
            if message is not None:
                yield {'time_us': timestamp, 'bus': bus_number, 'device': address,
                       'sequence': frame[2] >> 4, 'text': message}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcap', type=Path)
    parser.add_argument('--bus', type=int)
    parser.add_argument('--device', type=int)
    args = parser.parse_args()
    for message in messages(args.pcap, args.bus, args.device):
        print(json.dumps(message))


if __name__ == '__main__':
    main()
