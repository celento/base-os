"""Ordinary Terminal actions during a real incremental filesystem lease."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class StorageBusyTests(unittest.TestCase):
    def test_files_and_paint_preserve_actions_for_retry(self):
        source = (ROOT / 'src/kernel.c').read_text()
        names = ('files_storage_busy', 'fm_new_file', 'fm_rename_commit', 'do_new_folder', 'paint_write_named')
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-desktop-storage-') as temporary:
            directory = Path(temporary)
            (directory / 'desktop_storage_ops.inc').write_text(''.join(function(source, name) for name in names))
            output = directory / 'desktop-storage'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/desktop_storage_busy_host.c'), str(ROOT / 'src/file_view.c'),
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_terminal_write_feedback_and_read_only_work(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-terminal-storage-') as temporary:
            output = Path(temporary) / 'terminal-storage'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/term_storage_busy_host.c'),
                            str(ROOT / 'tests/net_stub.c'), str(ROOT / 'src/download.c'),
                            '-DDOWNLOAD_HOST_TEST', '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                    UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
