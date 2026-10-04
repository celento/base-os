"""Owned-window shell behavior with production helpers and bounded fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function
ROOT=Path(__file__).resolve().parents[1]
class NativeWindowShellTests(unittest.TestCase):
    def test_actual_shell_helpers(self):
        source=(ROOT/'src/kernel.c').read_text()
        names=('native_output_rows','native_output_columns','native_output_total',
               'native_output_scroll_by','native_output_toggle','native_task_key_dispatch',
               'native_window_complete','session_window_kind')
        with tempfile.TemporaryDirectory(prefix='baseos-native-shell-') as tmp:
            directory=Path(tmp)
            (directory/'native_window_shell_kernel.inc').write_text(''.join(function(source,name) for name in names))
            exe=directory/'shell'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-I',str(ROOT/'src'),'-I',str(directory),
                str(ROOT/'tests/native_window_shell_host.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
if __name__=='__main__':unittest.main()
