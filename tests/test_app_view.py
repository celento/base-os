"""Compiled app-view layout and ordinary ownership regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AppViewTests(unittest.TestCase):
    def test_explicit_view_ownership(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-view-host-') as directory:
            exe = Path(directory) / 'view'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/app_view_host.c'), str(ROOT / 'tests/net_stub.c'),
                            str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'), check=True)

    def test_actual_i386_layout(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-view-layout-') as directory:
            obj = Path(directory) / 'layout.o'
            subprocess.run([compiler, '-m32', '-std=gnu11', '-ffreestanding',
                            '-fno-pie', '-fno-pic', '-I', str(ROOT / 'src'), '-c',
                            str(ROOT / 'tests/app_view_layout.c'), '-o', str(obj)], check=True)
            result = subprocess.check_output(['nm', '-S', '--defined-only', str(obj)], text=True)
            sizes = {row.split()[-1]: int(row.split()[1], 16) for row in result.splitlines()}
            self.assertEqual(sizes, {
                'measured_terminal_text': 27432,
                'measured_app_canvas': 64032,
                'measured_view_metadata': 72,
                'measured_app_view': 64104,
                'measured_app_storage': 732288,
                'measured_view_offset': 219456,
                'measured_published_pixels': 512000,
            })
            self.assertLessEqual(sizes['measured_app_storage'], 0xC0000)
            self.assertLessEqual(sizes['measured_published_pixels'], 0x80000)

if __name__ == '__main__':
    unittest.main()
