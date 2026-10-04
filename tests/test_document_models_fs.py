"""Real Writer/Spreadsheet + adapter/coordinator durability on both IDE profiles."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class DocumentModelsFSTests(unittest.TestCase):
    def run_profile(self, large):
        with tempfile.TemporaryDirectory(prefix='baseos-document-models-') as tmp:
            output = Path(tmp) / 'models'
            command = [shutil.which('clang') or shutil.which('cc'), '-std=gnu11', '-O1', '-g',
                       '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       '-DWRITER_HOST_TEST', '-DSPREADSHEET_HOST_TEST', '-DGFX_HOST_TEST',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       '-fsanitize=address,undefined', '-I', str(ROOT / 'src')]
            if large:
                command += ['-DDOCUMENT_LARGE_PROFILE']
            command += [str(ROOT / 'tests/document_models_fs_host.c')]
            command += [str(ROOT / 'src' / name) for name in
                        ('writer.c', 'writer_codec.c', 'writer_pdf.c', 'sheet.c', 'sheet_model.c',
                         'sheet_codec.c', 'decimal.c', 'document_save.c', 'kernel_owner.c', 'gfx.c')]
            subprocess.run(command + ['-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                                              UBSAN_OPTIONS='halt_on_error=1'))
    def test_default(self):
        self.run_profile(False)
    def test_large(self):
        self.run_profile(True)
if __name__ == '__main__':
    unittest.main()
