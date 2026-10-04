"""Ordinary device samples through the exact production ordered ingress."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]

class InputIngressTests(unittest.TestCase):
    def test_ordered_device_samples(self):
        with tempfile.TemporaryDirectory(prefix='baseos-input-ingress-') as temporary:
            output = Path(temporary) / 'input-ingress'
            subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-O1', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/input_ingress_host.c'),
                            str(ROOT / 'src/input_ingress.c'), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], check=True, capture_output=True, text=True)
            self.assertIn('All ordered ingress checks passed.', result.stdout)

    def test_production_desktop_adapter(self):
        source = (ROOT / 'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-desktop-input-') as temporary:
            directory = Path(temporary)
            start = source.index('/* Desktop-only compatibility adapter')
            end = source.index('void kmain(void)', start)
            functions = function(source, 'input_gesture_ticks') + function(source, 'double_click')
            functions += function(source, 'win_resize_tick') + function(source, 'saver_start')
            (directory / 'desktop_input_kernel.inc').write_text(functions + source[start:end])
            output = directory / 'desktop-input'
            subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-function', '-O1', '-I', str(ROOT / 'src'),
                            '-I', str(directory), str(ROOT / 'tests/desktop_input_host.c'),
                            str(ROOT / 'src/input_ingress.c'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True)

if __name__ == '__main__':
    unittest.main()
