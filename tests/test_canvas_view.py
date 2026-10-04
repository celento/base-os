"""Shared renderer/input geometry, with deterministic host and i386 checks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CanvasViewTests(unittest.TestCase):
    def test_geometry_and_coordinates(self):
        with tempfile.TemporaryDirectory(prefix='baseos-canvas-view-') as temporary:
            output = Path(temporary) / 'canvas-view'
            subprocess.run([shutil.which('cc') or 'cc', '-std=gnu11', '-O1',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/canvas_view_host.c'),
                            str(ROOT / 'src/canvas_view.c'), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], check=True, capture_output=True, text=True)
            self.assertIn('widened coordinates passed.', result.stdout)

    def test_i386_without_compiler_runtime(self):
        with tempfile.TemporaryDirectory(prefix='baseos-canvas-view-i386-') as temporary:
            output = Path(temporary) / 'canvas-view.o'
            subprocess.run([shutil.which('gcc') or 'cc', '-std=gnu11', '-m32',
                            '-ffreestanding', '-Os', '-Wall', '-Wextra', '-Werror',
                            '-nostdlib', '-fno-pie', '-fno-pic', '-fno-stack-protector',
                            '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float',
                            '-I', str(ROOT / 'src'), '-c', str(ROOT / 'src/canvas_view.c'),
                            '-o', str(output)], check=True)
            undefined = subprocess.check_output(['nm', '-u', str(output)], text=True)
            self.assertEqual(undefined.strip(), '', 'geometry must link without libgcc/libc')


if __name__ == '__main__':
    unittest.main()
