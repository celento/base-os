"""Ordinary device samples through the exact production ordered ingress."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function
from desktop_source import desktop_input_defs, desktop_input_kernel

ROOT = Path(__file__).resolve().parents[1]

class InputIngressTests(unittest.TestCase):
    def test_ordered_device_samples(self):
        with tempfile.TemporaryDirectory(prefix='baseos-input-ingress-') as temporary:
            output = Path(temporary) / 'input-ingress'
            subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-O1', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/input_ingress_host.c'),
                            str(ROOT / 'src/kernel/input_ingress.c'), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], check=True, capture_output=True, text=True)
            self.assertIn('All ordered ingress checks passed.', result.stdout)

    def test_production_desktop_adapter(self):
        source = (ROOT / 'src/shell/desktop/desktop.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-desktop-input-') as temporary:
            directory = Path(temporary)
            (directory / 'desktop_input_defs.inc').write_text(desktop_input_defs(source))
            (directory / 'desktop_input_kernel.inc').write_text(desktop_input_kernel(source, function))
            output = directory / 'desktop-input'
            subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-function', '-O1', '-I', str(ROOT / 'src'),
                            '-I', str(directory), str(ROOT / 'tests/desktop_input_host.c'),
                            str(ROOT / 'src/kernel/input_ingress.c'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True)

    def test_terminal_draft_survives_input_loss(self):
        with tempfile.TemporaryDirectory(prefix='baseos-terminal-input-loss-') as temporary:
            output = Path(temporary) / 'terminal-input-loss'
            subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-function', '-O1', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/term_input_loss_host.c'), str(ROOT / 'src/kernel/input_ingress.c'),
                            str(ROOT / 'tests/net_stub.c'), str(ROOT / 'src/kernel/download.c'),
                            '-DDOWNLOAD_HOST_TEST', '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True)

if __name__ == '__main__':
    unittest.main()
