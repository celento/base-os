"""Real Sheet + document-save adapter, deterministic ordinary asynchronous service."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SheetAsyncTests(unittest.TestCase):
    def test_sanitized_async_model_workflows(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required for Spreadsheet async tests')
        with tempfile.TemporaryDirectory(prefix='baseos-sheet-async-') as directory:
            executable = pathlib.Path(directory) / 'sheet_async_host'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-DSPREADSHEET_HOST_TEST', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/sheet_async_host.c'),
                            str(ROOT / 'src/sheet_codec.c'), str(ROOT / 'src/sheet_model.c'),
                            str(ROOT / 'src/decimal.c'), str(ROOT / 'src/document_save.c'),
                            str(ROOT / 'src/kernel_owner.c'), '-o', str(executable)], check=True)
            env = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                       UBSAN_OPTIONS='halt_on_error=1')
            result = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE,
                                    env=env, check=True)
            self.assertIn('All Spreadsheet async host functional checks passed.', result.stdout)
            for section in ('native', 'failures', 'admission', 'exports', 'lifecycle', 'bounds', 'revisions'):
                self.assertIn(f'Spreadsheet async {section}:', result.stdout)


if __name__ == '__main__':
    unittest.main()
