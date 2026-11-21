#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack source-owned ELF sections into the rtw88 8821C firmware format."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
from pack_rtw import build
from parse_rtw import parse_header, split_sections
from format import Elf32

GEOMETRY = {"imem": (0x80030000, 0xF708), "dmem": (0x80200000, 0x7CE8),
            "emem": (0x80100000, 0xAC88)}
REQUIRED_SYMBOLS = ("_start", "g_diag", "g_events", "g_abi_results",
                    "__data_start", "__data_end", "__bss_start", "__bss_end",
                    "__stack_bottom", "__stack_top")


def audit_disassembly(text, elf, reserve_t9=False, core_ops=None):
    """Gate generated code separately from literal pools, using ELF symbols.

    This bounds compiler output; the hardware ABI checks still establish actual
    behavior. Generic binutils is not authoritative for the opaque entry opcode.
    """
    functions = [(s["address"] & ~1, (s["address"] & ~1) + s["size"])
                 for _, s in elf.symbol_entries if s["type"] == 2 and s["size"]]
    pools = elf.literal_pools()
    allowed = set("li lui addiu addu subu move neg not and or xor sll srl sra sllv srlv srav "
                  "slti sltiu slt sltu cmp cmpi lw sw lb lbu lh lhu sb sh b beqz bnez "
                  "bteqz btnez jal jalx jalr jr nop mfhi mflo".split())
    count, mnemonics, opaque = 0, set(), []
    core_used = {}
    for line in text.splitlines():
        address = re.match(r"^([0-9a-f]{8}):", line)
        if address is None:
            continue
        address = int(address[1], 16)
        if not any(lo <= address < hi for lo, hi in functions) or any(lo <= address < hi for lo, hi in pools):
            continue
        match = re.match(r"^[0-9a-f]{8}:\s+([0-9a-f]{4})(?:\s+[0-9a-f]{4})?\s+([.a-z][a-z0-9_.]*)", line)
        if match is None:
            raise ValueError(f"unrecognized code listing: {line}")
        if address == 0x8003000E and match[1] == "b98c":
            if core_ops and "b98c" in core_ops:
                core_used[hex(address)] = {"halfword": "b98c", "operation": core_ops["b98c"]}
                opaque.append("0x8003000e: fixed bootstrap Status write, qualified by interrupt foundation")
            else:
                opaque.append("0x8003000e: preserved opaque b98c")
            continue
        mnemonic = match[2]
        core_function = any(s["type"] == 2 and name.startswith("core_") and
                            (s["address"] & ~1) <= address < (s["address"] & ~1) + s["size"]
                            for name, s in elf.symbol_entries)
        if core_ops and core_function and match[1] in core_ops:
            core_used[hex(address)] = {"halfword": match[1], "operation": core_ops[match[1]]}
            count += 1
            continue
        if reserve_t9 and re.search(r"\bt9\b", line):
            raise ValueError(f"unqualified t9 register transfer at {address:#x}; reserve register 25")
        if mnemonic not in allowed:
            raise ValueError(f"unqualified compiler instruction at {address:#x}: {mnemonic}")
        mnemonics.add(mnemonic)
        count += 1
    if len(opaque) != 1 or count < 100:
        raise ValueError("incomplete diagnostic code audit")
    return {"instruction_count": count, "mnemonics": sorted(mnemonics),
            "qualified_core_operations": core_used,
            "literal_pools": len(pools), "opaque_boundaries": opaque,
            "profile": "conservative MIPS16 C; selective verified entry assembly"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bodies(elf):
    sections = {name: bytearray(size) for name, (_, size) in GEOMETRY.items()}
    allowed = {".text": "imem", ".diag_layout": "imem", ".data": "dmem", ".bss": "dmem"}
    for name, section in elf.sections.items():
        if not section["flags"] & 2 or not section["size"]:
            continue
        if name not in allowed:
            raise ValueError(f"unpackaged allocated section: {name}")
        region = allowed[name]
        base, size = GEOMETRY[region]
        start = section["address"] - base
        if start < 0 or start + section["size"] > size:
            raise ValueError(f"section out of range: {name}")
        if region == "dmem" and (section["address"] < 0x80200200 or
                                 section["address"] + section["size"] > 0x80205000):
            raise ValueError("data/BSS overlaps reserved prefix or stack")
        if name == ".bss":
            if section["type"] != 8:
                raise ValueError("BSS must be NOLOAD")
            continue  # The output is zero-initialized.
        sections[region][start:start + section["size"]] = section["data"]
    for name in REQUIRED_SYMBOLS:
        if name not in elf.symbols:
            raise ValueError(f"missing symbol {name}")
    entry = elf.symbols["_start"]["address"] & ~1
    if entry != GEOMETRY["imem"][0] or elf.entry & ~1 != entry:
        raise ValueError("entry must be first IMEM halfword")
    if (elf.symbols["__stack_bottom"]["address"], elf.symbols["__stack_top"]["address"]) != (0x80205000, 0x80206000):
        raise ValueError("unexpected stack extent")
    struct.pack_into("<I", sections["dmem"], 0x17C, entry | 1)
    elf.layout()  # reject a stale/mismatched reader layout before upload
    return {name: bytes(data) for name, data in sections.items()}


def verify(image, elf):
    info = parse_header(image)
    if info["file_size"] != info["expected_size"] or info["signature"] != 0x8821:
        raise ValueError("invalid packed image")
    expected = bodies(elf)
    sections = split_sections(image, info)
    for name, (address, size) in GEOMETRY.items():
        if info[name + "_addr"] != address or info[name + "_size"] != size:
            raise ValueError(f"changed proven layout: {name}")
        data = sections[name]
        xor = 0
        for value, in struct.iter_unpack("<H", data):
            xor ^= value
        if xor or data[:-8] != expected[name]:
            raise ValueError(f"checksum/composition mismatch: {name}")
    if info["feature"] or info["h2c_fmt_ver"]:
        raise ValueError("unexpected firmware offload or H2C format flag")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--compiler", type=Path)
    ap.add_argument("--disassembly", required=True, type=Path)
    ap.add_argument("--cflags", default="")
    ap.add_argument("--asflags", default="")
    ap.add_argument("--fw-version", required=True)
    ap.add_argument("--extra-source", action="append", type=Path, default=[])
    ap.add_argument("--reserve-t9", action="store_true")
    ap.add_argument("--core-ops", type=Path)
    args = ap.parse_args()
    data = args.elf.read_bytes()
    elf = Elf32(data)
    sections = bodies(elf)
    core_ops = json.loads(args.core_ops.read_text()) if args.core_ops else None
    audit = audit_disassembly(args.disassembly.read_text(), elf, args.reserve_t9, core_ops)
    audit["reserve_t9"] = args.reserve_t9
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    version = tuple(int(x) for x in args.fw_version.split("."))
    if len(version) != 3 or any(x < 0 or x > 255 for x in version):
        ap.error("firmware version must have three byte-sized components")
    image = build(sections["imem"], sections["dmem"], sections["emem"],
                  stamp=time.gmtime(epoch), version=version)
    verify(image, elf)
    report = {
        "format": 1, "time_utc": datetime.fromtimestamp(epoch, timezone.utc).isoformat(),
        "image": args.output.name, "image_sha256": sha(image), "elf_sha256": sha(data),
        "composition": "own ELF sections + generated entry pointer + zero fill",
        "layout": elf.layout(),
        "symbols": {name: elf.symbols[name] for name in REQUIRED_SYMBOLS},
        "sections": {name: {"address": GEOMETRY[name][0], "size": len(blob), "sha256": sha(blob)}
                     for name, blob in sections.items()},
        "source_sha256": {path.name: sha(path.read_bytes()) for path in sorted(HERE.iterdir())
                          if path.suffix in (".c", ".h", ".S", ".ld") or
                          path.name in ("Makefile", "pack.py", "format.py")},
        "cflags": args.cflags, "asflags": args.asflags, "instruction_audit": audit,
    }
    if args.compiler:
        report["compiler"] = subprocess.check_output([str(args.compiler), "--version"], text=True).splitlines()[0]
        report["compiler_sha256"] = sha(args.compiler.read_bytes())
    report["extra_source_sha256"] = {str(path): sha(path.read_bytes()) for path in args.extra_source}
    report["symbols"].update({name: symbol for name, symbol in elf.symbols.items()
                              if name in ("g_compat", "g_efuse_physical", "g_efuse_logical", "g_feature",
                                          "g_foundation", "g_context_snapshot", "g_receive", "g_station",
                                          "g_broader", "g_rate", "g_power", "g_lps",
                                          "g_beacon", "g_radio", "g_doze", "g_wowlan", "g_deep", "g_clock", "g_deep_resume", "g_deep_iqk")})
    args.output.write_bytes(image)
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {args.output} ({len(image)} bytes, firmware {args.fw_version})")


if __name__ == "__main__":
    main()
