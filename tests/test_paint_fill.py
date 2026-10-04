"""Ordinary Paint fills using current production input, flood and undo helpers."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class PaintFillTests(unittest.TestCase):
    def test_regions_boundaries_colors_and_undo(self):
        source = (ROOT / 'src/kernel.c').read_text()
        first = source.index('#define PAINT_W ')
        last = source.index('static void paint_init(void)', first)
        implementation = re.search(r'^#define TITLE_H\s+\d+', source, re.M).group() + '\n'
        implementation += source[first:last]
        implementation += ''.join(function(source, name) for name in (
            'paint_init', 'plot_lg', 'paint_stamp', 'bresenham_lg', 'rect_lg',
            'paint_flood', 'paint_canvas_geom', 'paint_mouse_logical',
            'paint_tool_cell', 'paint_brush_cell', 'paint_well_cell',
            'paint_well2_cell', 'handle_paint_click', 'paint_undo'))
        with tempfile.TemporaryDirectory(prefix='baseos-paint-fill-') as temporary:
            directory = Path(temporary)
            (directory / 'paint_fill_kernel.inc').write_text(implementation)
            binary = directory / 'paint-fill-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/paint_fill_host.c'), str(ROOT / 'src/history.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env=dict(
                os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
