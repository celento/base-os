"""Bounded Writer model/UI functional checks with real font rendering."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WriterTests(unittest.TestCase):
    def test_functional_workflows(self):
        with tempfile.TemporaryDirectory(prefix='baseos-writer-') as temporary:
            output = pathlib.Path(temporary) / 'writer_host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DWRITER_HOST_TEST', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/writer_host.c'), str(ROOT / 'src/writer.c'),
                            str(ROOT / 'src/document_save.c'), str(ROOT / 'src/kernel_owner.c'),
                            str(ROOT / 'src/writer_codec.c'), str(ROOT / 'src/writer_pdf.c'), '-o', str(output)], check=True)
            environment = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                               UBSAN_OPTIONS='halt_on_error=1')
            subprocess.run([str(output)], check=True, env=environment)


if __name__ == '__main__':
    unittest.main()
