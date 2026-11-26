# SPDX-License-Identifier: GPL-2.0-only
"""Diagnostic ABI and a small reader for our own ELF32 little-endian outputs."""
import struct

MAGIC = 0x31414944
LAYOUT_MAGIC = 0x314C4944
ABI_VERSION = 1
BAD_ADDRESS = 0xBADADD00
STATUS_FIELDS = (
    "magic", "abi_version", "struct_bytes", "fw_version", "state",
    "progress", "mailbox_reads", "invalid_requests", "last_error",
    "abi_result", "stack_ok", "led_state", "ring_head", "ring_count",
    "ring_capacity", "ring_addr", "ring_overwrites",
    "chip.raw_sys_cfg1", "chip.raw_sys_status1", "chip.raw_wl_bt_power",
    "chip.cut", "chip.vendor_id", "chip.test_chip", "chip.rf_paths",
    "chip.rom_revision", "chip.wifi_func_flag", "chip.bt_func_flag",
    "chip.bt_board_flag", "chip.bt_enabled", "chip.bt_supported",
)
EVENT_NAMES = {1: "boot", 2: "abi", 3: "chip", 4: "heartbeat",
               5: "bluetooth_disabled", 6: "host_interface", 255: "error"}


class Elf32:
    """No relocation processing: the fixed-address executable is already linked."""
    def __init__(self, data):
        if len(data) < 52:
            raise ValueError("short ELF")
        h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
        if h[0][:7] != b"\x7fELF\x01\x01\x01" or h[1:4] != (2, 8, 1):
            raise ValueError("need ELF32 little-endian MIPS executable")
        self.entry = h[4]
        if h[11] != 40 or not h[12] or h[13] >= h[12]:
            raise ValueError("unsupported ELF section headers")
        self.data = data
        headers = [struct.unpack("<10I", self.slice(h[6] + i * 40, 40))
                   for i in range(h[12])]
        names = self.slice(headers[h[13]][4], headers[h[13]][5])
        self.sections = {}
        for sh in headers:
            name = self.string(names, sh[0])
            self.sections[name] = {"type": sh[1], "flags": sh[2], "address": sh[3],
                                   "size": sh[5], "link": sh[6], "entry_size": sh[9],
                                   "data": b"" if sh[1] == 8 else self.slice(sh[4], sh[5])}
        self.symbols = {}
        self.symbol_entries = []  # Local names may repeat across C objects.
        for sh in headers:
            if sh[1] != 2:
                continue
            if sh[9] != 16 or sh[5] % 16 or sh[6] >= len(headers):
                raise ValueError("invalid symbol table")
            strings = headers[sh[6]]
            strings = self.slice(strings[4], strings[5])
            for offset in range(sh[4], sh[4] + sh[5], 16):
                name, value, size, info, _, section = struct.unpack("<IIIBBH", self.slice(offset, 16))
                if section:
                    symbol_name = self.string(strings, name)
                    symbol = {"address": value, "size": size, "type": info & 15}
                    self.symbol_entries.append((symbol_name, symbol))
                    self.symbols[symbol_name] = symbol

    def literal_pools(self):
        starts, ends = {}, {}
        for name, symbol in self.symbol_entries:
            if name.startswith('__pool_'):starts.setdefault(name[7:], []).append(symbol['address'])
            if name.startswith('__pend_'):ends.setdefault(name[7:], []).append(symbol['address'])
        pools = []
        for name, addresses in starts.items():
            for start in addresses:
                candidates = [end for end in ends.get(name, []) if end >= start]
                if not candidates:raise ValueError('literal pool has no end: '+name)
                pools.append((start, min(candidates)))
        return sorted(pools)

    def slice(self, start, size):
        if start < 0 or size < 0 or start + size > len(self.data):
            raise ValueError("ELF extent outside file")
        return self.data[start:start + size]

    @staticmethod
    def string(strings, offset):
        end = strings.find(b"\0", offset)
        if offset < 0 or end < 0:
            raise ValueError("invalid ELF string")
        return strings[offset:end].decode("ascii")

    def layout(self):
        data = self.sections[".diag_layout"]["data"]
        if len(data) != (6 + len(STATUS_FIELDS)) * 4:
            raise ValueError("diagnostic layout length mismatch")
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        if words[:2] != (LAYOUT_MAGIC, ABI_VERSION) or words[5] != len(STATUS_FIELDS):
            raise ValueError("diagnostic layout version mismatch")
        if words[2:5] != (120, 16, 64) or sorted(words[6:]) != list(range(0, 120, 4)):
            raise ValueError("unsupported diagnostic ABI")
        return {"abi_version": words[1], "status_bytes": words[2], "event_bytes": words[3],
                "ring_capacity": words[4], "fields": dict(zip(STATUS_FIELDS, words[6:]))}


def decode_status(data, layout):
    if len(data) != layout["status_bytes"]:
        raise ValueError("short diagnostic status")
    flat = {name: struct.unpack_from("<I", data, offset)[0]
            for name, offset in layout["fields"].items()}
    if (flat["magic"] != MAGIC or flat["abi_version"] != ABI_VERSION or
            flat["struct_bytes"] != len(data)):
        raise ValueError("firmware/reader ABI mismatch")
    if flat["ring_capacity"] != layout["ring_capacity"] or flat["ring_count"] > flat["ring_capacity"]:
        raise ValueError("invalid diagnostic ring metadata")
    result = {name: value for name, value in flat.items() if not name.startswith("chip.")}
    result["chip"] = {name[5:]: value for name, value in flat.items() if name.startswith("chip.")}
    return result
