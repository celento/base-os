"""Exact production name-dialog input/dispatch with real Writer export wrappers."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class WriterDialogTests(unittest.TestCase):
    def test_pdf_paper_keyboard_mouse_failure_retry_and_legacy_dialogs(self):
        source = (ROOT / 'src/kernel.c').read_text()
        names = ('namedlg_open', 'namedlg_hide', 'namedlg_close', 'namedlg_geom', 'namedlg_buttons',
                 'namedlg_commit', 'namedlg_click', 'namedlg_key')
        with tempfile.TemporaryDirectory(prefix='baseos-writer-dialog-') as tmp:
            directory = Path(tmp)
            (directory / 'writer_dialog_ops.inc').write_text(''.join(function(source, name) for name in names))
            compiler = shutil.which('clang') or shutil.which('cc')
            output = directory / 'writer_dialog_host'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined', '-DWRITER_HOST_TEST',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/writer_dialog_host.c'), str(ROOT / 'src/writer.c'),
                            str(ROOT / 'src/document_save.c'), str(ROOT / 'src/kernel_owner.c'),
                            str(ROOT / 'src/writer_codec.c'), str(ROOT / 'src/writer_pdf.c'),
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                                              UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
