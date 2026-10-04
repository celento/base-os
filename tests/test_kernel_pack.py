"""Ordinary valid packed-image fixtures. No corrupt inputs or fault injection."""
import ctypes
import ctypes.util
import pathlib
import struct
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from kernel_pack import LZ4, RAW, compress_lz4, decompress_lz4, pack_kernel, unpack_kernel
from layout import constants


class KernelPackTests(unittest.TestCase):
    def test_valid_literal_and_overlap_sequences(self):
        for raw in (b'', b'abc', bytes(range(15)), bytes(range(255)),
                    b'A' * 10000, bytes(range(256)) * 257,
                    b'BaseOS packed kernel\n' * 30000):
            packed = compress_lz4(raw)
            self.assertEqual(decompress_lz4(packed, len(raw)), raw)
            self.assertEqual(compress_lz4(raw), packed)
        # An ordinary length-1000 overlapping match at backward distance one.
        block = b'\x1fA\x01\x00' + bytes((255, 255, 255, 216)) + b'\x50HELLO'
        self.assertEqual(decompress_lz4(block, 1006), b'A' * 1001 + b'HELLO')
        # The largest representable backward distance with a valid final tail.
        raw = bytes((i * 37) & 255 for i in range(65535))
        extra = 65535 - 15
        block = b'\xf3' + b'\xff' * (extra // 255) + bytes((extra % 255,))
        block += raw + b'\xff\xff' + b'\x50FINAL'
        self.assertEqual(decompress_lz4(block, 65547), raw + raw[:7] + b'FINAL')

    def test_explicit_raw_and_lz4_envelopes(self):
        c = constants()
        raw = bytes(range(256)) * 1000
        for codec in (RAW, LZ4):
            packed = pack_kernel(raw, c, codec)
            self.assertEqual(packed[:4096], raw[:4096])
            self.assertEqual(struct.unpack_from('<I', packed, 4104)[0], codec)
            self.assertEqual(unpack_kernel(packed, c), raw)
            self.assertEqual(pack_kernel(raw, c, codec), packed)

    def test_initialized_image_above_old_disk_limit(self):
        c = constants()
        raw = (b'BASEOS-LARGER-INITIALIZED-CODE\x90\x90\x90\x90' * 24000)[:700000]
        self.assertGreater(len(raw), c['KERNEL_SECTORS'] * 512)
        self.assertLess(len(raw), c['STACK_BOTTOM'] - c['KERNEL_LOAD_ADDR'])
        packed = pack_kernel(raw, c)
        self.assertLess(len(packed), c['KERNEL_SECTORS'] * 512)
        self.assertEqual(unpack_kernel(packed, c), raw)

    def test_producer_capacity_boundaries(self):
        c = constants()
        ram_limit = c['STACK_BOTTOM'] - c['KERNEL_LOAD_ADDR']
        self.assertEqual(unpack_kernel(pack_kernel(bytes(ram_limit), c), c), bytes(ram_limit))
        with self.assertRaisesRegex(ValueError, 'RAM reservation'):
            pack_kernel(bytes(ram_limit + 1), c)
        raw_disk_limit = c['KERNEL_SECTORS'] * 512 - c['KERNEL_BOOTSTRAP_BYTES'] - 32
        self.assertEqual(len(pack_kernel(bytes(raw_disk_limit), c, RAW)),
                         c['KERNEL_SECTORS'] * 512)
        with self.assertRaisesRegex(ValueError, 'boot loader reservation'):
            pack_kernel(bytes(raw_disk_limit + 1), c, RAW)

    def test_actual_kernel_round_trip_and_repeatability(self):
        path = ROOT / 'build/kernel.bin'
        if not path.exists():
            self.skipTest('make first to check the actual linked kernel')
        raw = path.read_bytes()
        packed = pack_kernel(raw)
        self.assertEqual(unpack_kernel(packed), raw)
        self.assertEqual(pack_kernel(raw), packed)
        saved = ROOT / 'build/kernel.packed'
        if saved.exists():
            self.assertEqual(saved.read_bytes(), packed)

    def test_standard_lz4_decoder_when_available(self):
        name = ctypes.util.find_library('lz4')
        if not name:
            self.skipTest('optional system liblz4 is not installed')
        lib = ctypes.CDLL(name)
        lib.LZ4_decompress_safe.argtypes = (ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_int, ctypes.c_int)
        lib.LZ4_decompress_safe.restype = ctypes.c_int
        cases = [bytes(range(256)) * 1000, b'ABCD' * 175000]
        kernel = ROOT / 'build/kernel.bin'
        if kernel.exists():
            cases.append(kernel.read_bytes())
        for raw in cases:
            packed = compress_lz4(raw)
            output = ctypes.create_string_buffer(len(raw))
            result = lib.LZ4_decompress_safe(packed, output, len(packed), len(raw))
            self.assertEqual(result, len(raw))
            self.assertEqual(output.raw, raw)


if __name__ == '__main__':
    unittest.main()
