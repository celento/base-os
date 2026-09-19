import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from update_image import update
from layout import constants


class ImageTests(unittest.TestCase):
    def test_preserve_volume_upgrade_and_backup(self):
        c = constants()
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            image, boot, kernel = [directory / p for p in ('disk.img', 'boot.bin', 'kernel.bin')]
            boot.write_bytes(bytes(510) + b'\x55\xaa')
            kernel.write_bytes(b'kernel')
            old = bytearray(2880 * 512)
            old[c['FS_DISK_LBA'] * 512:] = b'X' * (len(old) - c['FS_DISK_LBA'] * 512)
            image.write_bytes(old)
            update(image, boot, kernel)
            new = image.read_bytes()
            self.assertEqual(len(new), c['DISK_SECTORS'] * 512)
            self.assertEqual(new[c['FS_DISK_LBA'] * 512:len(old)], old[c['FS_DISK_LBA'] * 512:])
            self.assertEqual(next(directory.glob('*.bak')).read_bytes(), old)
            # Rebuild after changing kernel bytes; preserve both slots.
            marker = bytearray(new)
            marker[c['FS_SECOND_LBA'] * 512 + 100] = 17
            image.write_bytes(marker)
            kernel.write_bytes(b'changed kernel')
            update(image, boot, kernel)
            self.assertEqual(image.read_bytes()[c['FS_DISK_LBA'] * 512:], marker[c['FS_DISK_LBA'] * 512:])

    def test_locked_image_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, boot, kernel = [d / p for p in ('disk', 'boot', 'kernel')]
            image.write_bytes(bytes(2880 * 512))
            boot.write_bytes(bytes(510) + b'\x55\xaa')
            kernel.write_bytes(b'x')
            child = subprocess.Popen([sys.executable, '-c',
                'import fcntl,sys; f=open(sys.argv[1],"r+b"); '
                'fcntl.lockf(f,fcntl.LOCK_EX); print("locked",flush=True); sys.stdin.read()',
                str(image)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline().strip(), b'locked')
                with self.assertRaises(BlockingIOError): update(image, boot, kernel)
                self.assertEqual(len(image.read_bytes()), 2880 * 512)
                self.assertFalse(list(d.glob('*.bak')))
            finally:
                child.communicate(timeout=5)

    def test_reject_bad_inputs_without_modifying_image(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, boot, kernel = [d / p for p in ('disk', 'boot', 'kernel')]
            image.write_bytes(b'valuable data')
            boot.write_bytes(bytes(510) + b'\x55\xaa')
            kernel.write_bytes(b'x')
            with self.assertRaises(ValueError): update(image, boot, kernel)
            self.assertEqual(image.read_bytes(), b'valuable data')
            image.write_bytes(bytes(2880 * 512))
            kernel.write_bytes(bytes(constants()['KERNEL_SECTORS'] * 512 + 1))
            with self.assertRaises(ValueError): update(image, boot, kernel)
            self.assertEqual(image.read_bytes(), bytes(2880 * 512))


class NativeTests(unittest.TestCase):
    def run_native(self, name, extra=()):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory() as temp:
            exe = pathlib.Path(temp) / name
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / f'{name}.c'), *map(str, extra), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_graphics(self): self.run_native("gfx_host")
    def test_features(self): self.run_native("features_host")
    def test_floppy(self): self.run_native("persist_host")
    def test_bootinfo(self): self.run_native("bootinfo_host")
    def test_filesystem(self): self.run_native('fs_host')
    def test_rtc(self): self.run_native('rtc_host', ['-DRTC_HOST_TEST', ROOT / 'src/rtc.c'])

if __name__ == '__main__': unittest.main()
