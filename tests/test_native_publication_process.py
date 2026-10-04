"""Exercise the production native interrupt publication boundaries on the host.

The dispatcher and task state transitions are extracted without rewriting their
logic. Only the two privileged interrupt-flag instructions around fs_sync are
replaced; the publication checks never execute that unrelated syscall.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from process_host_extract import extract

ROOT = Path(__file__).resolve().parents[1]


class NativePublicationProcessTests(unittest.TestCase):
    def test_production_interrupt_boundaries(self):
        source = (ROOT / 'src/process.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-native-publication-process-') as temporary:
            directory = Path(temporary)
            extract(source, directory, 'native_publication_process')
            executable = directory / 'native_publication_process_host'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/native_publication_process_host.c'),
                            str(ROOT / 'src/physmem.c'), str(ROOT / 'src/bootinfo.c'),
                str(ROOT / 'src/executable.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, env=dict(
                os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
