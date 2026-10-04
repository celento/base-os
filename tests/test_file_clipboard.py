"""Identity-safe Files clipboard workflows on ordinary valid volumes."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class FileClipboardTests(unittest.TestCase):
    def test_ordinary_file_operations(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='baseos-file-clipboard-') as temporary:
            executable = pathlib.Path(temporary) / 'file-clipboard'
            subprocess.run([
                compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                str(ROOT / 'tests/file_clipboard_host.c'), str(ROOT / 'src/file_clipboard.c'),
                '-o', str(executable),
            ], check=True)
            environment = dict(os.environ,
                               ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                               UBSAN_OPTIONS='halt_on_error=1')
            subprocess.run([str(executable)], check=True, env=environment)


if __name__ == '__main__':
    unittest.main()
