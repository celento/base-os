"""Native update classification and exact production partial-render equivalence."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class NativeRenderTests(unittest.TestCase):
    def run_host(self, name, prepare=None, extra=()):
        with tempfile.TemporaryDirectory(prefix='baseos-native-render-') as temporary:
            directory = Path(temporary)
            if prepare:
                prepare(directory)
            output = directory / name
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests' / (name + '.c')), *extra,
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(
                os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_update_classification(self):
        self.run_host('native_dirty_host', extra=(str(ROOT / 'tests/net_stub.c'),
                      str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST'))

    def test_explicit_owner_canvas_size(self):
        self.run_host('native_canvas_size_host', extra=(str(ROOT / 'tests/net_stub.c'),
                      str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST'))

    def test_render_equivalence_and_guards(self):
        def prepare(directory):
            source = (ROOT / 'src/kernel.c').read_text()
            for name in ('gui_draw_window', 'cursor_restore', 'cursor_save_draw', 'draw_ui'):
                source = source.replace('void ' + name + '(', 'static void ' + name + '(')
            names = ('close_box_pos', 'min_box_pos', 'draw_min_box', 'draw_close_box',
                     'draw_sb_arrow', 'draw_thumb', 'draw_scrollbars', 'gui_draw_window',
                     'term_text', 'term_canvas_geometry', 'draw_app_canvas', 'draw_term_canvas', 'draw_term',
                     'cursor_restore', 'cursor_save_draw', 'win_front',
                     'client_scene_stable', 'partial_client_ready', 'window_content_hidden',
                     'term_canvas_hidden', 'wins_by_z', 'draw_one_window', 'draw_ui')
            code = ''.join(function(source, name) for name in names)
            code += 'enum { TERM_RENDER_NONE, TERM_RENDER_FULL, TERM_RENDER_CANVAS };\n'
            code += function(source, 'term_task_render_action')
            (directory / 'native_render_kernel.inc').write_text(code)
        self.run_host('native_render_host', prepare, extra=(str(ROOT / 'src/canvas_view.c'),))


if __name__ == '__main__':
    unittest.main()
