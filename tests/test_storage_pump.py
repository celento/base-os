"""Exact production storage_poll bounds; does not replace normal QEMU evidence."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class StoragePumpTests(unittest.TestCase):
    def test_actual_production_pump_contract(self):
        source = (ROOT / 'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-storage-pump-') as temporary:
            folder = Path(temporary)
            # Compile the actual function body, never a copied scheduling loop.
            (folder / 'storage_pump.inc').write_text(function(source, 'storage_poll'))
            executable = folder / 'storage-pump'
            compiler = shutil.which('clang') or shutil.which('cc')
            self.assertIsNotNone(compiler, 'C compiler required')
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(folder),
                            str(ROOT / 'tests/storage_pump_host.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
