"""Owned-window shell behavior with production helpers and bounded fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import unittest
from test_editor_binding import function
ROOT=Path(__file__).resolve().parents[1]
class NativeWindowShellTests(unittest.TestCase):
    def test_monitor_collector_control_geometry(self):
        sys.path.insert(0,str(ROOT/'tools'))
        from native_window_input_test import ShellFont
        from native_window_shell_input_test import monitor_layout
        font=ShellFont()
        for window in ((2,38,1276,636),(100,70,520,472),(20,50,900,650)):
            x,y,w,h=window
            points=monitor_layout(window,font)
            for px,py in points['tabs']+[points['show'],points['stop'],points['close']]:
                self.assertTrue(x<px<x+w and y+32<py<y+h)
            self.assertLess(points['show'][0],points['stop'][0])
            self.assertEqual(points['stop'][1]+42,y+160)
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
