#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an rtw88 3081 image with a 64-byte header and section checksums."""
import struct
import time

FW_HDR_SIZE = 64
CHKSUM_SIZE = 8
BIT_EMEM = 1 << 4


def xor16_checksum(payload: bytes) -> bytes:
    x = 0
    for i in range(0, len(payload) & ~1, 2):
        x ^= struct.unpack_from("<H", payload, i)[0]
    return struct.pack("<H", x) + bytes(6)


def pad4(b: bytes) -> bytes:
    if len(b) % 4:
        b += bytes(4 - (len(b) % 4))
    return b


def stamp_header(hdr: bytearray, when: time.struct_time,
                 version: tuple[int, int, int] | None = None) -> None:
    """Replace the host-visible version and build time. Section layout stays."""
    if version is not None:
        ver, sub, idx = version
        struct.pack_into("<HBB", hdr, 0x04, ver, sub, idx)
    struct.pack_into(
        "<BBBBH", hdr, 0x10,
        when.tm_mon, when.tm_mday, when.tm_hour, when.tm_min, when.tm_year,
    )


def build(imem: bytes, dmem: bytes, emem: bytes, *,
          stamp: time.struct_time, version: tuple[int, int, int]) -> bytes:
    imem, dmem, emem = pad4(imem), pad4(dmem), pad4(emem)
    hdr = bytearray(FW_HDR_SIZE)
    struct.pack_into("<H", hdr, 0, 0x8821)
    hdr[0x18] = 0x08 | (BIT_EMEM if emem else 0)
    struct.pack_into("<I", hdr, 0x20, 0x80200000)
    struct.pack_into("<I", hdr, 0x3C, 0x80030000)
    struct.pack_into("<I", hdr, 0x38, 0x80100000)
    stamp_header(hdr, stamp, version)
    struct.pack_into("<I", hdr, 0x24, len(dmem))
    struct.pack_into("<I", hdr, 0x30, len(imem))
    struct.pack_into("<I", hdr, 0x34, len(emem))
    return (
        bytes(hdr)
        + dmem + xor16_checksum(dmem)
        + imem + xor16_checksum(imem)
        + ((emem + xor16_checksum(emem)) if emem else b"")
    )
