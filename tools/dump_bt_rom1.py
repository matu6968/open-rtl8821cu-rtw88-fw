#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read the RTL8821CU Bluetooth ROM 1 mapping over Linux HCI."""

import argparse
from hashlib import sha256
from pathlib import Path
import socket
import struct
import sys
import time

READ_REG16 = 0xfc61
READ_ROM_VERSION = 0xfc6d
ROM_START = 0x80000000
ROM_END = 0x80070000
USB_DEVICES = Path('/sys/bus/usb/devices')
HCI_DEVICES = Path('/sys/class/bluetooth')


def controller(port):
    usb = USB_DEVICES / port
    vendor = (usb / 'idVendor').read_text().strip()
    product = (usb / 'idProduct').read_text().strip()
    if (vendor, product) != ('0bda', 'c820'):
        raise ValueError(f'{port} is not a USB 0bda:c820 adapter')
    interface = (USB_DEVICES / f'{port}:1.0').resolve(strict=True)
    indexes = [int(path.name[3:]) for path in HCI_DEVICES.glob('hci[0-9]*')
               if (path / 'device').resolve() == interface]
    if len(indexes) != 1:
        raise ValueError(f'expected one Bluetooth controller at {port}:1.0')
    return indexes[0], (usb / 'devnum').read_text().strip()


def command(channel, opcode, parameters):
    channel.sendall(struct.pack('<BHB', 0x01, opcode, len(parameters)) + parameters)
    deadline = time.monotonic() + 3
    while (remaining := deadline - time.monotonic()) > 0:
        channel.settimeout(remaining)
        response = channel.recv(260)
        if len(response) < 6 or response[0] != 0x04 or response[2] != len(response) - 3:
            raise ValueError(f'malformed HCI event {response.hex()}')
        if response[1] == 0x0f and len(response) == 7:
            status, _, actual_opcode = struct.unpack_from('<BBH', response, 3)
            if actual_opcode == opcode and status:
                raise ValueError(f'HCI 0x{opcode:04x} status 0x{status:02x}')
        if response[1] == 0x0e and struct.unpack_from('<H', response, 4)[0] == opcode:
            return response[6:]
    raise TimeoutError(f'HCI 0x{opcode:04x} command timed out')


def read_halfword(channel, address):
    reply = command(channel, READ_REG16, struct.pack('<BI', 0x10, address))
    if len(reply) != 3 or reply[0]:
        raise ValueError(f'ROM read at 0x{address:08x} returned {reply.hex()}')
    return reply[1:]


def read_region(channel, base, length):
    data = bytearray(length)
    for offset in range(0, length, 2):
        try:
            data[offset:offset + 2] = read_halfword(channel, base + offset)
        except OSError as error:
            raise OSError(f'ROM read at 0x{base + offset:08x} failed: {error}') from error
        if offset and offset % 0x4000 == 0:
            print(f'Read 0x{offset:x} of 0x{length:x} bytes', file=sys.stderr)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--usb-port', required=True, help='USB sysfs port, for example 1-2')
    parser.add_argument('--base', type=lambda value: int(value, 0), default=ROM_START,
                        help='first controller address (default 0x80000000)')
    parser.add_argument('--length', type=lambda value: int(value, 0),
                        help='byte count (default through 0x8006ffff)')
    args = parser.parse_args()
    if Path(args.usb_port).name != args.usb_port:
        parser.error('--usb-port must be a sysfs port name')
    length = args.length if args.length is not None else ROM_END - args.base
    if (args.base & 1 or length <= 0 or length & 1
            or not ROM_START <= args.base < args.base + length <= ROM_END):
        parser.error(f'use a nonempty halfword-aligned range within 0x{ROM_START:x}–0x{ROM_END:x}')
    if sys.stdout.isatty():
        parser.error('redirect the binary output to a file')

    try:
        index, devnum = controller(args.usb_port)
        with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as channel:
            channel.bind((index,))
            # The kernel expects the trailing padding of struct hci_ufilter.
            filt = struct.pack('=IIIHxx', 1 << 4, (1 << 0x0e) | (1 << 0x0f), 0, 0)
            channel.setsockopt(socket.SOL_HCI, socket.HCI_FILTER, filt)
            channel.settimeout(3)
            if command(channel, READ_ROM_VERSION, b'') != b'\x00\x01':
                raise ValueError('expected Bluetooth ROM revision 1')
            for address in (args.base, args.base + length - 2):
                word = read_halfword(channel, address)
                print(f'0x{address:08x} = {word.hex()}', file=sys.stderr)
            print(f'Capturing ROM 1 on hci{index}, USB device {devnum}', file=sys.stderr)
            first = read_region(channel, args.base, length)
            if controller(args.usb_port) != (index, devnum):
                raise ValueError('Bluetooth controller changed during the first read')
            print('Rereading the same range', file=sys.stderr)
            second = read_region(channel, args.base, length)
            if controller(args.usb_port) != (index, devnum):
                raise ValueError('Bluetooth controller changed during the second read')
            if first != second:
                offset = next(i for i, (a, b) in enumerate(zip(first, second)) if a != b)
                raise ValueError(f'ROM reads differ at 0x{args.base + offset:08x}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')

    sys.stdout.buffer.write(first)
    print(f'Matching reads, {len(first)} bytes, SHA-256 {sha256(first).hexdigest()}', file=sys.stderr)


if __name__ == '__main__':
    main()
