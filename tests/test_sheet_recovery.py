"""Ordinary Spreadsheet recovery workflows using exact kernel session functions.

The generated includes select production functions without the hardware desktop;
real fs.c and history.c run against deterministic in-memory block devices.
"""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, name):
    pattern = r'^static [^\n]+\b' + name + r'\([^;]*?\)\s*\{'
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError('Missing production function: ' + name)
    start = match.start()
    depth, end = 1, match.end()
    # These selected functions have no brace characters in string literals.
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


class SheetRecoveryTests(unittest.TestCase):
    def test_session_recovery_admission_and_examples(self):
        source = (ROOT / 'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-sheet-recovery-host-') as temporary:
            directory = pathlib.Path(temporary)
            first = source.index('typedef struct {\n    char buf[EDIT_BUF_SIZE];')
            last = source.index('static WindowState *window_state', first)
            (directory / 'editor_kernel_types.inc').write_text(source[first:last])
            first = source.index('#define fm_cwd ')
            last = source.index('/* Folder contexts belong', first)
            operations = source[first:last]
            for name in ('fm_set_cwd', 'fm_checked_cwd', 'edit_record', 'edit_undo',
                         'edit_sel_collapse', 'edit_clear', 'edit_load', 'edit_source_unchanged',
                         'edit_storage_busy', 'edit_write_to', 'edit_write_named', 'edit_save'):
                operations += function(source, name)
            (directory / 'editor_kernel_ops.inc').write_text(operations)
            first = source.index('typedef struct { int open,kind,x,y,w,h,min,z,caret;')
            last = source.index('int program_key(void)', first)
            (directory / 'editor_kernel_session.inc').write_text(source[first:last])
            output = directory / 'editor_binding_host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined', '-DSPREADSHEET_HOST_TEST',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/sheet_recovery_host.c'), str(ROOT / 'src/history.c'),
                            str(ROOT / 'src/sheet.c'), str(ROOT / 'src/sheet_model.c'),
                            str(ROOT / 'src/sheet_codec.c'), str(ROOT / 'src/decimal.c'),
                            str(ROOT / 'src/example_sheet.c'), str(ROOT / 'src/native_sync.c'),
                            str(ROOT / 'src/document_save.c'), str(ROOT / 'src/kernel_owner.c'),
                            '-o', str(output)], check=True)
            env = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                       UBSAN_OPTIONS='halt_on_error=1')
            subprocess.run([str(output)], check=True, env=env)


if __name__ == '__main__':
    unittest.main()
