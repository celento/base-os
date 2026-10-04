"""Ordinary layout and full-reservation image checks, without guest fault probes."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from layout import constants
from update_image import install_kernel, install_packed_kernel, kernel_offset, update
from kernel_pack import RAW, pack_kernel, unpack_kernel


class KernelLayoutTests(unittest.TestCase):
    def test_relocated_ranges_and_unchanged_arenas(self):
        c = constants()
        self.assertEqual(c['KERNEL_STAGE_ADDR'], 0x10000)
        self.assertEqual(c['KERNEL_LOAD_ADDR'], 0x100000)
        self.assertLessEqual(c['KERNEL_STAGE_ADDR'] + c['KERNEL_SECTORS'] * 512,
                             c['KERNEL_STAGE_LIMIT'])
        self.assertEqual(c['STACK_TOP'] - c['STACK_BOTTOM'], 65536)
        self.assertLessEqual(c['STACK_TOP'], c['FB_BASE'])
        self.assertEqual((c['FS_DISK_LBA'], c['FS_SECOND_LBA'], c['FS_DISK_SECTORS']),
                         (384, 2784, 2400))
        self.assertEqual((c['FS_BASE'], c['FS_CAPACITY'], c['WRITER_BASE'], c['WRITER_CAPACITY']),
                         (0x300000, 0x10000, 0x310000, 0x1F0000))
        self.assertEqual((c['TASK_INTERRUPT_STACK_BASE'], c['TASK_INTERRUPT_STACK_CAPACITY']),
                         (0x30E0000, 65536))
        self.assertEqual(c['RAM_REQUIRED_END'], 0x3F00000)
        self.assertEqual((c['SHEET_BASE'], c['SHEET_CAPACITY']), (0x720000, 0x2E0000))
        self.assertEqual(c['AUDIO_DMA_BASE'] + c['AUDIO_DMA_CAPACITY'], c['SHEET_BASE'])
        self.assertEqual(c['SHEET_BASE'] + c['SHEET_CAPACITY'], c['DESK_CACHE'])
        self.assertLessEqual(2824636, c['SHEET_CAPACITY'])

    def test_published_canvas_reservation(self):
        c = constants()
        self.assertEqual((c['NATIVE_CANVAS_BASE'], c['NATIVE_CANVAS_CAPACITY']),
                         (0x600000, 0x80000))
        self.assertLessEqual(c['PAINT_MEM'] + c['PAINT_CAPACITY'], c['NATIVE_CANVAS_BASE'])
        self.assertLessEqual(8 * 320 * 200, c['NATIVE_CANVAS_CAPACITY'])
        self.assertLessEqual(c['NATIVE_CANVAS_BASE'] + c['NATIVE_CANVAS_CAPACITY'], c['DMA_BASE'])
        self.assertLessEqual(c['FB_BASE'], c['NATIVE_CANVAS_BASE'])
        self.assertLessEqual(c['NATIVE_CANVAS_BASE'] + c['NATIVE_CANVAS_CAPACITY'], c['RAM_REQUIRED_END'])

    def test_full_tail_reservation_preserves_both_snapshots(self):
        c = constants()
        data = bytearray(c['DISK_SECTORS'] * 512)
        lo, hi = c['FS_DISK_LBA'] * 512, c['KERNEL_EXT_LBA'] * 512
        data[lo:hi] = b'V' * (hi - lo)
        raw_size = c['KERNEL_SECTORS'] * 512 - c['KERNEL_BOOTSTRAP_BYTES'] - 32
        raw = bytes((i * 37 + i // 512) & 255 for i in range(raw_size))
        kernel = pack_kernel(raw, c, codec=RAW)
        self.assertEqual(len(kernel), c['KERNEL_SECTORS'] * 512)
        self.assertEqual(unpack_kernel(kernel, c), raw)
        install_packed_kernel(data, kernel, c)
        self.assertEqual(data[lo:hi], b'V' * (hi - lo))
        primary = c['KERNEL_PRIMARY_SECTORS'] * 512
        self.assertEqual(data[512:512 + primary] + data[hi:], kernel)
        self.assertEqual(kernel_offset(len(kernel) - 1, c), len(data) - 1)
        for offset in (0, 65535, 65536, primary - 1, primary, len(kernel) - 1):
            self.assertEqual(data[kernel_offset(offset, c)], kernel[offset])

    def test_explicit_packed_update_matches_raw_api(self):
        c = constants()
        raw = bytes(range(256)) * 2000
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            boot = directory / 'boot.bin'
            boot.write_bytes(bytes(510) + b'\x55\xaa')
            kernel = directory / 'kernel.bin'
            kernel.write_bytes(raw)
            packed = directory / 'kernel.packed'
            packed.write_bytes(pack_kernel(raw, c))
            raw_image, packed_image = directory / 'raw.img', directory / 'packed.img'
            update(raw_image, boot, kernel)
            update(packed_image, boot, packed, packed=True)
            self.assertEqual(raw_image.read_bytes(), packed_image.read_bytes())

    def test_legacy_upgrade_retains_existing_bytes_and_backup(self):
        c = constants()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            image, boot, kernel = [directory / n for n in ('old.img', 'boot.bin', 'kernel.bin')]
            old = bytearray(2880 * 512)
            lo = c['FS_DISK_LBA'] * 512
            old[lo:] = b'L' * (len(old) - lo)
            image.write_bytes(old)
            boot.write_bytes(bytes(510) + b'\x55\xaa')
            kernel.write_bytes(b'K' * (c['KERNEL_SECTORS'] * 512))
            update(image, boot, kernel)
            new = image.read_bytes()
            self.assertEqual(new[lo:len(old)], old[lo:])
            self.assertEqual(next(directory.glob('*.bak')).read_bytes(), old)
            packed = pack_kernel(kernel.read_bytes(), c)
            first = c['KERNEL_PRIMARY_SECTORS'] * 512
            code = new[512:512 + first] + new[c['KERNEL_EXT_LBA'] * 512:]
            self.assertEqual(unpack_kernel(code[:len(packed)], c), kernel.read_bytes())
            self.assertEqual(code[len(packed):], bytes(len(code) - len(packed)))


if __name__ == '__main__':
    unittest.main()
