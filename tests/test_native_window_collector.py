"""Host-only collector geometry/decoding/volume checks. Never starts QEMU."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import native_window_input_test as collector


def logical_pointer(font, logical):
    width, height = logical
    image = np.zeros((height, width, 3), dtype=np.uint8)
    rows = [f'POINTER {width}X{height}', 'S=00000000:0000002A', 'X=-12 Y=60',
            'B=0 G=7 T=2', 'RESET=3 CANCEL=2', 'DRAG=0 DONE=8']
    for row, (y, color) in zip(rows, ((2, 8), (9, 7), (16, 7), (23, 7), (30, 6), (37, 8))):
        mask = font.bitmap(row)
        image[y:y+5, 2:2+mask.shape[1]][mask] = collector.COLORS[color]
    return image


class NativeWindowCollectorTests(unittest.TestCase):
    def test_exact_fractional_native_pointer_decode(self):
        font = collector.NativeFont()
        for window in ((320, 116, 640, 480), (2, 38, 1276, 636), (640, 38, 636, 636)):
            for logical in ((160, 100), (320, 200)):
                with self.subTest(window=window, logical=logical):
                    screen = np.full((720, 1280, 3), 32, dtype=np.uint8)
                    x, y, w, h = collector.native_viewport(window, logical)
                    screen[y:y+h, x:x+w] = collector.resample(logical_pointer(font, logical), w, h)
                    state = font.pointer(screen, window, logical)
                    self.assertEqual((state['done'], state['reset'], state['x'], state['sequence']), (8, 3, -12, 42))
                    sx, sy = collector.point(state, 70, 60)
                    self.assertEqual(((sx-x)*logical[0]//w, (sy-y)*logical[1]//h), (70, 60))
                    # An ordinary obscuring shell overlay must never decode as
                    # a complete authoritative native status frame.
                    screen[y+3, x+3] = (255, 255, 255)
                    with self.assertRaises(AssertionError): font.pointer(screen, window, logical)

    def test_geometry_rounding_and_downscale_refusal(self):
        self.assertEqual(collector.native_viewport((320, 116, 640, 480)), (329, 193, 622, 388))
        viewport = collector.native_viewport((0, 0, 240, 180), (320, 200))
        self.assertEqual(viewport, (40, 71, 160, 100))
        with self.assertRaises(AssertionError):
            collector.reconstruct(np.zeros((200, 300, 3), dtype=np.uint8), viewport, (320, 200))

    def test_exact_shell_font_diagnostic_match(self):
        font = collector.ShellFont(); screen = np.full((130, 640, 3), 32, dtype=np.uint8)
        text = 'WINDOW LOG ROW 63'; mask = font.bitmap(text)
        screen[41:59, 37:37+mask.shape[1]][mask] = 232
        self.assertTrue(font.matches(screen, 37, 41, text))
        self.assertTrue(font.contains(screen, (0, 0, 640, 130), text))
        self.assertFalse(font.contains(screen, (0, 0, 640, 130), 'WINDOW LOG ROW 15'))

    def test_fixture_document_build_and_owned_budget(self):
        with tempfile.TemporaryDirectory(prefix='baseos-window-document-host-') as temporary:
            output = Path(temporary) / 'document.bex'
            collector.build_app(ROOT/'tests/native_window_document_app.c', output, format='bex2',
                                window='native-v1', workspace_bytes=0, stack_bytes=16384)
            image = output.read_bytes(); header = collector._bex2_header(image)
            self.assertEqual(header[3], 1)
            self.assertEqual(header[10:14], (0, 16384, 1, 2))
            self.assertGreater(collector.declared_pages(image), 6)
            self.assertLess(collector.declared_pages(image), 32)
            hosted = Path(temporary)/'hosted.bex'
            collector.build_app(ROOT/'tests/native_window_document_app.c', hosted)
            self.assertEqual(hosted.read_bytes()[:4], b'BEX1')
            self.assertLessEqual(hosted.stat().st_size, 49152)
            expected = collector.document_bytes()
            self.assertEqual(len(expected), 16384)
            self.assertTrue(all(expected[i] == 10 for i in range(79, len(expected), 80)))

    def test_fresh_volume_and_stopped_bytes(self):
        apps = {'counter.bex': (collector.FROZEN/'counter.bex').read_bytes()}
        with tempfile.TemporaryDirectory(prefix='baseos-window-volume-host-') as temporary:
            disk = Path(temporary) / 'data.img'
            record = collector.make_volume(disk, 'default', apps)
            _, _, nodes = collector.load(disk.read_bytes())
            self.assertEqual(nodes[collector.resolve(nodes, '/Programs/counter.bex')]['data'], apps['counter.bex'])
            self.assertEqual(nodes[collector.resolve(nodes, '/Documents/counter-8.txt')]['data'], b'108\n')
            before = record['sha256']
            with self.assertRaises(AssertionError): collector.make_volume(disk, 'default', apps)
            self.assertEqual(collector.file_record(disk)['sha256'], before)

    def test_prepare_never_constructs_guest_and_retains_failure(self):
        with tempfile.TemporaryDirectory(prefix='baseos-window-prepare-host-') as temporary:
            root = Path(temporary); build = root/'build'; build.mkdir()
            for name in collector.CORE_ARTIFACTS: (build/name).write_bytes(b'Host-only placeholder '+name.encode())
            held = root/'hosted.bex'; held.write_bytes(b'Unqualified placeholder')
            with mock.patch.object(collector, 'Session', side_effect=AssertionError('QEMU must not start')):
                with self.assertRaisesRegex(AssertionError, 'qualified extraction binary'):
                    collector.prepare(build, root/'evidence', held, profiles=())
            manifest = json.loads((root/'evidence/manifest.json').read_text())
            self.assertEqual(manifest['status'], 'PREPARATION FAILED')
            self.assertTrue(manifest['preparation_only'])
            self.assertIn('failure', manifest)
            self.assertTrue(all(value == 'NOT RUN' for value in manifest['lanes'].values()))

    def test_all_frozen_artifacts_are_rechecked(self):
        with tempfile.TemporaryDirectory(prefix='baseos-window-freeze-host-') as temporary:
            directory = Path(temporary)
            build = {}
            for name in collector.CORE_ARTIFACTS:
                path = directory/name; path.write_bytes(b'Unexecuted host fixture '+name.encode())
                build[name] = collector.file_record(path)
            manifest = dict(build=build, apps={}, source={})
            collector.verify_inputs(manifest)
            (directory/'baseos.img').write_bytes(b'Changed ordinary fixture image')
            with self.assertRaisesRegex(AssertionError, 'baseos.img'):
                collector.verify_inputs(manifest)

if __name__ == '__main__':
    unittest.main()
