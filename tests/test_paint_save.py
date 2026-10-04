"""Ordinary Paint saves using extracted production helpers and the real filesystem."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class PaintSaveTests(unittest.TestCase):
    def test_new_names_capacity_rollback_lease_and_exact_remount(self):
        source = (ROOT / 'src/kernel.c').read_text()
        implementation = ''.join(function(source, name) for name in
                                 ('paint_init', 'unique_untitled_pbm',
                                  'paint_write_named', 'paint_save'))
        with tempfile.TemporaryDirectory(prefix='baseos-paint-save-') as temporary:
            directory = Path(temporary)
            (directory / 'paint_kernel.inc').write_text(implementation)
            binary = directory / 'paint-save-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/paint_save_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                    UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
