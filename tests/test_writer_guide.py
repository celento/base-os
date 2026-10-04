"""The shipped original quick start matches its reproducible native generator."""
import pathlib
import struct
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_writer_example import document


class WriterGuideTests(unittest.TestCase):
    def test_exact_native_asset_and_current_controls(self):
        raw = (ROOT / 'assets/examples/writer-guide.bwr').read_bytes()
        self.assertEqual(raw, document())
        magic, version, flags, length, reserved = struct.unpack_from('<4sHHII', raw)
        self.assertEqual((magic, version, flags, reserved), (b'BWR1', 1, 0, 0))
        self.assertLessEqual(length, 32768)
        self.assertEqual(len(raw), 18 + 3 * length)
        text = raw[16:16 + length]
        styles = raw[16 + length:17 + 2 * length]
        paragraphs = raw[17 + 2 * length:]
        self.assertTrue(all(c == 10 or 32 <= c <= 126 for c in text))
        self.assertTrue(all(not (value & ~7) for value in styles + paragraphs))
        self.assertTrue(all(not value or i == 0 or text[i - 1] == 10 for i, value in enumerate(paragraphs)))
        for control in (b'Ctrl+Shift+P', b'Letter or A4', b'Ctrl+F filters', b'start /Programs/docstats.bex'):
            self.assertIn(control, text)
