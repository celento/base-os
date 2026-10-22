"""Native update classification and exact production partial-render equivalence."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function
from desktop_source import CHROME_MACROS, block, defines

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
                      str(ROOT / 'src/kernel/download.c'), '-DDOWNLOAD_HOST_TEST'))

    def test_explicit_owner_canvas_size(self):
        self.run_host('native_canvas_size_host', extra=(str(ROOT / 'tests/net_stub.c'),
                      str(ROOT / 'src/kernel/download.c'), '-DDOWNLOAD_HOST_TEST'))

    def test_render_equivalence_and_guards(self):
        def prepare(directory):
            source = (ROOT / 'src/shell/desktop/desktop.c').read_text()
            for name in ('gui_draw_window', 'cursor_restore', 'cursor_save_draw', 'draw_ui'):
                source = source.replace('void ' + name + '(', 'static void ' + name + '(')
            # Production chrome geometry (style metrics), caption, scrollbar,
            # window flag, role color and Terminal color macros, window kinds
            # and the window record. The host fixture includes these first.
            defs = defines(source, CHROME_MACROS + (
                'INFO_H', 'SB', 'WIN_INFO', 'WIN_SCROLL', 'WIN_NOCLOSE', 'WIN_INACTIVE',
                'WIN_NOMENU', 'WIN_MAXED', 'WIN_DIALOG', 'CAP_H', 'CAP_W', 'CAP_CLOSE_W',
                'TERM_PAD', 'TERM_BG', 'TERM_FG', 'TERM_ANSI', 'ui_text_dim', 'ui_border'))
            defs += block(source, 'enum WinKind {', '};\n')
            defs += block(source, 'typedef struct {\n    int kind;\n    int x, y, w, h;', '} Win;\n')
            # Chrome state for the window being drawn and the hover targets.
            defs += block(source, '/* App icon (AI_*) shown in the title bar', '/* ---------- Shared desktop controls',
                          include_end=False)
            (directory / 'native_render_defs.inc').write_text(defs)
            code = 'static int caption_pointer_ok(void);\n'
            code += re.search(r'^enum \{ TL_PLAIN[^\n]*\n', source, re.M).group()
            names = ('cap_y', 'close_box_x', 'max_box_x', 'min_box_x', 'draw_caption',
                     'draw_win_control', 'draw_vscroll', 'draw_scrollbars', 'gui_draw_window',
                     'term_line_kind', 'term_char_ink', 'term_line_chunk', 'term_body',
                     'term_scroll_marker', 'term_canvas_geometry', 'draw_app_canvas', 'draw_term_canvas',
                     'draw_term', 'cursor_restore', 'cursor_save_draw', 'win_front',
                     'client_scene_stable', 'partial_client_ready', 'window_content_hidden',
                     'term_canvas_hidden', 'wins_by_z', 'win_at', 'caption_pointer_ok',
                     'win_is_dialog', 'draw_one_window', 'draw_ui')
            code += ''.join(function(source, name) for name in names)
            code += 'enum { TERM_RENDER_NONE, TERM_RENDER_FULL, TERM_RENDER_CANVAS };\n'
            code += function(source, 'term_task_render_action')
            (directory / 'native_render_kernel.inc').write_text(code)
        self.run_host('native_render_host', prepare, extra=(str(ROOT / 'src/lib/canvas_view.c'),))


if __name__ == '__main__':
    unittest.main()
