"""Stable Terminal frame publication and its production renderer read path."""
from pathlib import Path
import unittest
from test_editor_binding import function
import test_native_render

ROOT = Path(__file__).resolve().parents[1]

class NativePublicationTests(unittest.TestCase):
    run_host = test_native_render.NativeRenderTests.run_host
    # Reuse only the sanitizer runner; existing render tests remain independent.
    def test_published_pixels_and_geometry(self):
        def prepare(directory):
            source = (ROOT / 'src/kernel.c').read_text()
            code = ''.join(function(source, name) for name in
                           ('term_canvas_geometry', 'draw_term_canvas'))
            (directory / 'native_publication_render.inc').write_text(code)
        for rectangle in (False, True):
            with self.subTest(rectangle=rectangle):
                extra = (str(ROOT / 'tests/net_stub.c'), str(ROOT / 'src/download.c'),
                         '-DDOWNLOAD_HOST_TEST')
                if rectangle:
                    extra += ('-DNATIVE_RECTANGLE_CALLBACK',)
                self.run_host('native_publication_host', prepare, extra)

if __name__ == '__main__':
    unittest.main()
