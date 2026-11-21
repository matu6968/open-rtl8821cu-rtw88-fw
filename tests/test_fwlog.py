# SPDX-License-Identifier: GPL-2.0-only
import ctypes
import os
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import decode_fwlog as decode


class FirmwareLogs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(work.cleanup)
        lib = Path(work.name) / 'log_model.so'
        src = ROOT / 'src'
        subprocess.run([
            os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
            '-Werror', '-DDIAG_HOST_TEST', '-shared', '-fPIC', '-Wl,--no-undefined',
            '-I', str(src), str(ROOT / 'tests/log_model.c'),
            *(str(src / name) for name in (
                'compat/control.c', 'compat/efuse.c', 'diagnostic/chip.c',
                'station/packet.c', 'fwlog/fwlog.c',
            )),
            '-o', str(lib),
        ], check=True)
        cls.model = ctypes.CDLL(str(lib))
        cls.model.model_emit.argtypes = [ctypes.c_char_p]

    def setUp(self):
        self.model.model_reset()

    def drain(self, polls=16):
        for _ in range(polls):
            self.model.model_poll()
        frame = (ctypes.c_uint8 * 14)()
        return [bytes(frame[:self.model.model_copy_frame(i, frame)])
                for i in range(self.model.model_frame_count())]

    def messages(self, polls=16):
        decoder = decode.Decoder()
        return [message for frame in self.drain(polls)
                if (message := decoder.consume(frame)) is not None]

    def test_board_profiles_from_factory_and_function_bits(self):
        combo = 'RTL8821CU: Bluetooth not yet supported'
        unknown = 'RTL8821C: variant unknown'
        cases = (
            (0x20, 0, 1, 0, 1, combo),
            (0x22, 1, 1, 0, 1, combo),
            (0x01, 0, 1, 0, 1, 'RTL8811CU: Wi-Fi only'),
            (0xff, 0, 1, 0, 1, unknown),
            (0xff, 1, 1, 0, 1, unknown),
            (0x01, 1, 1, 0, 1, unknown),
            (0x20, 1, 0, 0, 0, unknown),
            (0x20, 1, 1, 1, 0, unknown),
        )
        for board, bt_function, valid, timeout, ready, message in cases:
            with self.subTest(board=board, bt_function=bt_function, valid=valid, timeout=timeout):
                self.model.model_reset()
                self.model.model_boot(board, bt_function, valid, timeout)
                self.assertEqual(self.model.model_ready(), ready)
                self.assertEqual(self.model.model_bad_writes(), 0)
                self.assertEqual(self.drain(), [])
                self.model.model_iqk(0, 0x00010000)
                self.model.model_iqk(0, 0)
                self.assertEqual(self.messages(), [message])

    def test_iqk_reports_one_failure_per_request(self):
        self.model.model_iqk(0x50, 0x11)
        self.model.model_iqk(0x28, 0x11)
        self.model.model_iqk(0, 0x10010)
        self.model.model_iqk(0, 0x10000)
        self.assertEqual(self.messages(), [
            'IQK: calibration timed out (status 0x50)',
            'IQK: request failed (status 0x28)',
            'IQK: calibration failed (TX/RX mask 0x10)',
        ])

    def test_busy_transport_preserves_sequence_and_exact_chunk_terminator(self):
        self.model.model_iqk(0, 0)
        self.model.model_emit(b'x' * 11)
        self.model.model_set_busy(1)
        self.assertEqual(self.drain(), [])
        self.model.model_set_busy(0)
        first, last = self.drain()
        self.assertEqual((first[1], last[1]), (254, 255))
        self.assertEqual((first[2] & 15, last[2] & 15), (1, 0))
        self.assertEqual((len(first), len(last)), (14, 3))

    def test_rate_trace_remains_opt_in(self):
        self.model.model_iqk(0, 0)
        self.model.model_rate_trace()
        self.assertEqual(self.model.fw_log_h2c(0x20149, 0), 1)
        self.model.model_rate_trace()
        self.assertEqual(self.model.fw_log_h2c(0x20049, 0), 1)
        self.model.model_rate_trace()
        self.assertEqual(self.messages(), ['Rate: increased'])

    def test_mac_reset_discards_partial_message(self):
        self.model.model_iqk(0, 0)
        self.model.model_emit(b'x' * 12)
        self.assertEqual(len(self.drain(1)), 1)
        self.model.fw_log_transport_lost()
        self.model.model_iqk(0, 0)
        self.model.model_emit(b'new message')
        self.assertEqual(self.messages(), ['new message'])


class Receiver(unittest.TestCase):
    def test_outer_sequence_wrap_and_interleaved_c2h(self):
        decoder = decode.Decoder()
        self.assertIsNone(decoder.consume(b'\x00\xfe\xf1' + b'a' * 11))
        self.assertIsNone(decoder.consume(b'\x0c\xff'))
        self.assertEqual(decoder.consume(b'\x00\x00\xf0b'), 'a' * 11 + 'b')
        self.assertEqual(decoder.consume(b'\x00\x01\x00c'), 'c')

    def test_packet_loss_and_invalid_countdown_discard_until_new_message(self):
        decoder = decode.Decoder()
        self.assertIsNone(decoder.consume(b'\x00\x00\x13' + b'x' * 11))
        self.assertIsNone(decoder.consume(b'\x00\x02\x11' + b'x' * 11))
        self.assertIsNone(decoder.consume(b'\x00\x03\x10x'))
        self.assertIsNone(decoder.consume(b'\x00\x04\x22' + b'x' * 11))
        self.assertIsNone(decoder.consume(b'\x00\x05\x20x'))
        self.assertEqual(decoder.consume(b'\x00\x06\x30y'), 'y')

    def test_usb_capture_isolates_devices_and_starts_new_epoch_on_download(self):
        def bulk(address, sequence, metadata, text):
            frame = bytes((0, sequence, metadata)) + text
            payload = struct.pack('<6I', len(frame), 0, 1 << 28, 0, 0, 0) + frame
            usb = bytearray(64)
            struct.pack_into('<BBBBH', usb, 8, ord('C'), 3, 0x81, address, 2)
            struct.pack_into('<iII', usb, 28, 0, len(payload), len(payload))
            return usb + payload

        reset = bytearray(65)
        struct.pack_into('<BBBBH', reset, 8, ord('S'), 2, 0, 4, 2)
        reset[40:48] = b'\x40\x05\x80\x00\x00\x00\x01\x00'
        reset[64] = 1
        records = (bulk(4, 1, 0x11, b'x' * 11), bulk(5, 1, 0x10, b'first'), reset,
                   bulk(4, 0, 0x10, b'second'))
        with tempfile.TemporaryDirectory() as work:
            pcap = Path(work) / 'usb.pcap'
            with pcap.open('wb') as output:
                output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 220))
                for i, record in enumerate(records):
                    output.write(struct.pack('<IIII', 0, i, len(record), len(record)))
                    output.write(record)
            self.assertEqual([(message['device'], message['text']) for message in decode.messages(pcap)],
                             [(5, 'first'), (4, 'second')])
