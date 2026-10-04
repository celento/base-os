"""Ordinary value-only document adapter boundaries with real FS/native_sync."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DocumentSaveTests(unittest.TestCase):
    def test_real_coordinator_and_disk(self):
        with tempfile.TemporaryDirectory(prefix='baseos-document-save-') as tmp:
            output = Path(tmp) / 'document-save'
            subprocess.run([shutil.which('clang') or shutil.which('cc'), '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/document_save_host.c'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                                              UBSAN_OPTIONS='halt_on_error=1'))

if __name__ == '__main__':
    unittest.main()
