"""Exact desktop continuation/name functions with real Writer and Sheet models."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function
ROOT = Path(__file__).resolve().parents[1]

class DocumentSaveUITests(unittest.TestCase):
    def test_exact_pending_dispatch_and_continuations(self):
        source = (ROOT / 'src/kernel.c').read_text()
        names = ('edit_close_cancel', 'edit_close_valid', 'document_finish', 'document_request',
                 'document_save_info', 'document_destination_valid', 'document_wait_for_save',
                 'document_persistence_poll', 'namedlg_open', 'namedlg_hide', 'namedlg_close',
                 'writer_save_document', 'spreadsheet_save_document', 'edit_close_choose', 'namedlg_commit')
        with tempfile.TemporaryDirectory(prefix='baseos-document-ui-') as tmp:
            directory = Path(tmp)
            (directory / 'document_save_ui_ops.inc').write_text(''.join(function(source, name) for name in names))
            output = directory / 'document-ui'
            command = [shutil.which('clang') or shutil.which('cc'), '-std=gnu11', '-O1', '-g',
                       '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       '-DWRITER_HOST_TEST', '-DSPREADSHEET_HOST_TEST', '-fsanitize=address,undefined',
                       '-I', str(ROOT / 'src'), '-I', str(directory), str(ROOT / 'tests/document_save_ui_host.c')]
            command += [str(ROOT / 'src' / name) for name in
                        ('writer.c', 'writer_codec.c', 'writer_pdf.c', 'sheet.c', 'sheet_model.c',
                         'sheet_codec.c', 'decimal.c', 'document_save.c', 'kernel_owner.c')]
            subprocess.run(command + ['-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                                              UBSAN_OPTIONS='halt_on_error=1'))
if __name__ == '__main__':
    unittest.main()
