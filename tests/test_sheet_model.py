"""Ordinary bounded spreadsheet operations with host sanitizers; no QEMU."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SheetModelTests(unittest.TestCase):
    def run_harness(self, source, marker):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'A host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='baseos-sheet-model-') as directory:
            executable = pathlib.Path(directory) / 'sheet_model_host'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'src'), str(ROOT / 'tests' / source),
                            str(ROOT / 'src/sheet_model.c'), str(ROOT / 'src/decimal.c'),
                            '-o', str(executable)], check=True)
            env = dict(os.environ)
            env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, env=env)
            self.assertIn(marker, result.stdout)

    def test_sanitized_model(self):
        self.run_harness('sheet_model_host.c', 'Sheet model tests passed')

    def test_sanitized_range_workload(self):
        self.run_harness('sheet_performance_host.c', '72000 range references')


if __name__ == '__main__':
    unittest.main()
