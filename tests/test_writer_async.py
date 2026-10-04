"""Ordinary Writer actions with the real adapter and deterministic async service."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class WriterAsyncTests(unittest.TestCase):
    def test_submitted_revisions_exports_lifetimes(self):
        with tempfile.TemporaryDirectory(prefix='baseos-writer-async-') as tmp:
            directory = Path(tmp)
            # Direct unit-level finite-counter fixture; no production test API.
            source = directory / 'writer.c'
            source.write_text((ROOT / 'src/writer.c').read_text() + '''
void writer_unit_revision_counter(unsigned epoch, unsigned counter) {
    state.next_revision = (DocumentRevision){epoch, counter};
}
''')
            output = directory / 'writer-async'
            subprocess.run([shutil.which('clang') or shutil.which('cc'), '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-DWRITER_HOST_TEST',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/writer_async_host.c'), str(source),
                            str(ROOT / 'src/document_save.c'), str(ROOT / 'src/kernel_owner.c'),
                            str(ROOT / 'src/writer_codec.c'), str(ROOT / 'src/writer_pdf.c'),
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                                              UBSAN_OPTIONS='halt_on_error=1'))

if __name__ == '__main__':
    unittest.main()
