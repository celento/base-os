"""Deterministic hosted endpoint core, without executing guest or privileged code."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract
ROOT = Path(__file__).resolve().parents[1]
class NativeUiTests(unittest.TestCase):
    def test_production_core(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-ui-') as temporary:
            output=Path(temporary)/'native-ui'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g',
                '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'src'),
                str(ROOT/'tests/native_ui_host.c'),str(ROOT/'src/native_ui.c'),
                str(ROOT/'src/canvas_view.c'),'-o',str(output)],check=True)
            subprocess.run([str(output)],check=True,env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
    def test_production_process_boundary(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-ui-process-') as temporary:
            directory=Path(temporary)
            extract((ROOT/'src/process.c').read_text(),directory,'native_platform')
            output=directory/'native-ui-process'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g',
                '-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',
                '-I',str(ROOT/'src'),'-I',str(directory),
                str(ROOT/'tests/native_ui_process_host.c'),str(ROOT/'src/native_ui.c'),
                str(ROOT/'src/canvas_view.c'),str(ROOT/'src/physmem.c'),str(ROOT/'src/bootinfo.c'),
                str(ROOT/'src/executable.c'),'-o',str(output)],check=True)
            subprocess.run([str(output)],check=True,env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
if __name__=='__main__':unittest.main()
