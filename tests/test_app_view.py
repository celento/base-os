"""Compiled app-view layout and ordinary ownership regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract

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

    def test_real_process_publication_lifetime(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-view-process-') as temporary:
            directory = Path(temporary)
            extract((ROOT / 'src/process.c').read_text(), directory, 'process_lifetime', scheduler=True)
            executable = directory / 'view-process'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/app_view_process_host.c'), str(ROOT / 'src/physmem.c'),
                            str(ROOT / 'src/bootinfo.c'), str(ROOT / 'src/executable.c'),
                            '-o', str(executable)], check=True)
            for ram in (64, 128):
                subprocess.run([str(executable), str(ram)], check=True, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

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
                'measured_view_metadata': 112,
                'measured_app_view': 64144,
                'measured_app_storage': 763808,
                'measured_app_log': 3900,
                'measured_view_offset': 219456,
                'measured_published_pixels': 512000,
            })
            self.assertLessEqual(sizes['measured_app_storage'], 0xC0000)
            self.assertLessEqual(sizes['measured_published_pixels'], 0x80000)

if __name__ == '__main__':
    unittest.main()
