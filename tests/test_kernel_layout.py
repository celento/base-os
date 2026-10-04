"""Ordinary layout and full-reservation image checks, without guest fault probes."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from layout import constants
from update_image import install_kernel, kernel_offset, update


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

    def test_full_tail_reservation_preserves_both_snapshots(self):
        c = constants()
        data = bytearray(c['DISK_SECTORS'] * 512)
        lo, hi = c['FS_DISK_LBA'] * 512, c['KERNEL_EXT_LBA'] * 512
        data[lo:hi] = b'V' * (hi - lo)
        kernel = bytes((i * 37 + i // 512) & 255 for i in range(c['KERNEL_SECTORS'] * 512))
        install_kernel(data, kernel, c)
        self.assertEqual(data[lo:hi], b'V' * (hi - lo))
        primary = c['KERNEL_PRIMARY_SECTORS'] * 512
        self.assertEqual(data[512:512 + primary] + data[hi:], kernel)
        self.assertEqual(kernel_offset(len(kernel) - 1, c), len(data) - 1)
        for offset in (0, 65535, 65536, primary - 1, primary, len(kernel) - 1):
            self.assertEqual(data[kernel_offset(offset, c)], kernel[offset])

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
            self.assertEqual(new[-512:], b'K' * 512)


if __name__ == '__main__':
    unittest.main()
