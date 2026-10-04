"""Ordinary device samples through the exact production ordered ingress."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

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

if __name__ == '__main__':
    unittest.main()
