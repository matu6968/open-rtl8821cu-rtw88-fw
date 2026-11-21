#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Parse an rtw88 3081 firmware image (64-byte header + DMEM/IMEM/EMEM)."""
import argparse
import struct
from pathlib import Path

FW_HDR_SIZE = 64
FW_HDR_CHKSUM_SIZE = 8
BIT_EMEM = 1 << 4


def parse_header(data: bytes) -> dict:
    if len(data) < FW_HDR_SIZE:
        raise ValueError("file shorter than 64-byte header")
    sig, category, function, version, subver, subidx = struct.unpack_from(
        "<HBBHBB", data, 0
    )
    feature = struct.unpack_from("<I", data, 0x0C)[0]
    month, day, hour, minute, year = struct.unpack_from("<BBBBH", data, 0x10)
    mem_usage = data[0x18]
    h2c_fmt_ver = struct.unpack_from("<H", data, 0x1C)[0]
    dmem_addr, dmem_size = struct.unpack_from("<II", data, 0x20)
    imem_size, emem_size, emem_addr, imem_addr = struct.unpack_from(
        "<IIII", data, 0x30
    )
    has_emem = bool(mem_usage & BIT_EMEM)
    dmem_total = dmem_size + FW_HDR_CHKSUM_SIZE
    imem_total = imem_size + FW_HDR_CHKSUM_SIZE
    emem_total = (emem_size + FW_HDR_CHKSUM_SIZE) if has_emem else 0
    expected = FW_HDR_SIZE + dmem_total + imem_total + emem_total
    return {
        "signature": sig,
        "category": category,
        "function": function,
        "version": version,
        "subversion": subver,
        "subindex": subidx,
        "feature": feature,
        "date": f"{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}",
        "mem_usage": mem_usage,
        "h2c_fmt_ver": h2c_fmt_ver,
        "dmem_addr": dmem_addr,
        "dmem_size": dmem_size,
        "imem_addr": imem_addr,
        "imem_size": imem_size,
        "emem_addr": emem_addr,
        "emem_size": emem_size,
        "has_emem": has_emem,
        "dmem_total": dmem_total,
        "imem_total": imem_total,
        "emem_total": emem_total,
        "expected_size": expected,
        "file_size": len(data),
    }


def split_sections(data: bytes, info: dict) -> dict:
    off = FW_HDR_SIZE
    dmem = data[off : off + info["dmem_total"]]
    off += info["dmem_total"]
    imem = data[off : off + info["imem_total"]]
    off += info["imem_total"]
    emem = b""
    if info["has_emem"]:
        emem = data[off : off + info["emem_total"]]
    return {"dmem": dmem, "imem": imem, "emem": emem}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("-o", "--outdir", type=Path, default=None)
    args = ap.parse_args()
    data = Path(args.firmware).read_bytes()
    info = parse_header(data)
    for k, v in info.items():
        if isinstance(v, int) and k not in (
            "category",
            "function",
            "subversion",
            "subindex",
            "h2c_fmt_ver",
            "has_emem",
        ):
            print(f"{k:16s} 0x{v:08x} ({v})")
        else:
            print(f"{k:16s} {v}")
    if info["expected_size"] != info["file_size"]:
        print("WARNING: size mismatch")
    secs = split_sections(data, info)
    outdir = args.outdir
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)
        stem = Path(args.firmware).stem
        for name, blob in secs.items():
            if not blob:
                continue
            path = outdir / f"{stem}_{name}.bin"
            path.write_bytes(blob)
            csum = blob[-FW_HDR_CHKSUM_SIZE:] if len(blob) >= FW_HDR_CHKSUM_SIZE else b""
            print(f"wrote {path} {len(blob)} bytes checksum={csum.hex()}")
            if len(blob) >= FW_HDR_CHKSUM_SIZE:
                body = blob[:-FW_HDR_CHKSUM_SIZE]
                bpath = outdir / f"{stem}_{name}_body.bin"
                bpath.write_bytes(body)
                print(f"wrote {bpath} {len(body)} bytes (no checksum)")


if __name__ == "__main__":
    main()
