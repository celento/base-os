"""Deterministic hosted endpoint core, without executing guest or privileged code."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract
from test_editor_binding import function
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
    def test_production_desktop_route(self):
        source=(ROOT/'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-native-ui-desktop-') as temporary:
            directory=Path(temporary)
            names=('input_gesture_ticks','double_click','win_resize_tick','saver_start',
                'native_canvas_geometry','native_host_snapshot','native_host_acquired','native_host_focus','desktop_native_pointer')
            code=''.join(function(source,name) for name in names)
            start=source.index('/* Desktop-only compatibility adapter')
            code+=source[start:source.index('void kmain(void)',start)]
            (directory/'desktop_input_kernel.inc').write_text(code)
            output=directory/'desktop-ui'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g',
                '-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',
                '-I',str(ROOT/'src'),'-I',str(directory),
                str(ROOT/'tests/native_ui_desktop_host.c'),str(ROOT/'src/native_ui.c'),
                str(ROOT/'src/canvas_view.c'),str(ROOT/'src/input_ingress.c'),
                '-o',str(output)],check=True)
            subprocess.run([str(output)],check=True,env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
    def test_finite_token_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-ui-limits-') as temporary:
            output=Path(temporary)/'limits'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g',
                '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'src'),
                str(ROOT/'tests/native_ui_limits_host.c'),str(ROOT/'src/canvas_view.c'),
                '-o',str(output)],check=True)
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
