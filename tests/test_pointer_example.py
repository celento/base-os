"""Deterministic production SDK example with ordinary hosted-service results."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PointerExampleTests(unittest.TestCase):
    def test_production_example(self):
        with tempfile.TemporaryDirectory(prefix='baseos-pointer-example-') as temporary:
            output = Path(temporary) / 'pointer-example'
            subprocess.run([
                shutil.which('clang') or 'cc', '-std=c11', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I', str(ROOT / 'sdk'), str(ROOT / 'tests/pointer_example_host.c'),
                '-o', str(output),
            ], check=True)
            subprocess.run([str(output)], check=True, env=dict(
                os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
