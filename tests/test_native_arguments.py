"""Normal startup-document and streaming example checks, without guest faults."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeArgumentsTests(unittest.TestCase):
    def native(self, name, extra=()):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-native-arguments-') as directory:
            exe = Path(directory) / name
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            '-I', str(ROOT / 'sdk'), str(ROOT / 'tests' / f'{name}.c'),
                            *map(str, extra), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, env=dict(os.environ,
                           ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_terminal_document_arguments(self):
        self.native('native_arguments_host', [ROOT / 'tests/net_stub.c',
                    ROOT / 'src/download.c', '-DDOWNLOAD_HOST_TEST'])

    def test_docstats_streaming_and_selection(self):
        self.native('docstats_host')


if __name__ == '__main__':
    unittest.main()
