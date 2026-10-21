"""Ordinary valid packed-image fixtures. No corrupt inputs or fault injection."""
import ctypes
import ctypes.util
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from kernel_pack import (LZ4, LZMA, RAW, compress_lz4, compress_lzma, decompress_lz4,
                         pack_kernel, unpack_kernel)
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

    def test_explicit_raw_lz4_and_lzma_envelopes(self):
        c = constants()
        raw = bytes(range(256)) * 1000
        for codec in (RAW, LZ4, LZMA):
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


class BootLzmaTests(unittest.TestCase):
    """The C decoder that runs at boot, checked against Python's lzma encoder."""

    def test_boot_decoder_matches_reference(self):
        compiler = shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('no host C compiler')
        cases = {'repeat': b'BaseOS packed kernel\n' * 30000,
                 'mixed': bytes((i * 37 + i // 511) & 255 for i in range(200000)),
                 'tiny': b'A' * 100}
        kernel = ROOT / 'build/kernel.bin'
        if kernel.exists():
            cases['kernel'] = kernel.read_bytes()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            binary = directory / 'boot_lzma'
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] \
                if compiler.endswith('clang') else []
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            *flags, '-I', str(ROOT / 'src'), str(ROOT / 'tests/boot_lzma_host.c'),
                            '-o', str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
            for label, raw in cases.items():
                with self.subTest(case=label):
                    (directory / 'raw').write_bytes(raw)
                    (directory / 'packed').write_bytes(compress_lzma(raw))
                    result = subprocess.run([str(binary), str(directory / 'packed'),
                                             str(directory / 'raw')], env=env,
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_linked_decoder_is_position_independent(self):
        objects = ROOT / 'build/boot_lzma.o'
        if not objects.exists():
            self.skipTest('make first to check the linked boot decoder')
        relocations = subprocess.run(['objdump', '-r', '-j', '.text.entry', str(objects)], check=True,
                                     capture_output=True, text=True).stdout
        self.assertNotIn('R_386', relocations)
        sections = subprocess.run(['objdump', '-h', str(objects)], check=True,
                                  capture_output=True, text=True).stdout
        self.assertIn('.text.entry', sections)
        for line in sections.splitlines():
            parts = line.split()
            if len(parts) > 2 and parts[1] in ('.data', '.bss', '.rodata', '.text'):
                self.assertEqual(int(parts[2], 16), 0, line)


if __name__ == '__main__':
    unittest.main()
