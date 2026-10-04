"""Ordinary display formatting and versioned native metadata interoperability."""
import csv
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SheetFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required for spreadsheet format tests')
        cls.temp = tempfile.TemporaryDirectory(prefix='baseos-sheet-format-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        executable = cls.directory / 'sheet_format_host'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'src'), str(ROOT / 'tests/sheet_format_host.c'),
                        str(ROOT / 'src/sheet_codec.c'), str(ROOT / 'src/sheet_model.c'),
                        str(ROOT / 'src/decimal.c'), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        cls.host_result = subprocess.run([str(executable), str(cls.directory)], check=True,
                                         text=True, capture_output=True, env=env)

    def test_display_rounding_metadata_and_roundtrip(self):
        self.assertIn('Spreadsheet format tests passed', self.host_result.stdout)
        self.assertIn('Maximum spreadsheet native v2: 332868 bytes', self.host_result.stdout)

    def test_exact_v2_wire_layout(self):
        data = (self.directory / 'formatted.bsh').read_bytes()
        self.assertEqual(struct.unpack_from('<4sHHHHI', data),
                         (b'BSH1', 2, 0, 128, 26, 5))
        widths = struct.unpack_from('<26H', data, 16)
        self.assertEqual(widths, (160,) + (104,) * 24 + (48,))
        expected = [
            (0, 2, 1, b'+001.2350'),
            (1, 3, 2, b'=A1*2'),
            (2, 2, 3, b'.125'),
            (26, 1, 2, b'0012'),
            (3327, 0, 1, b''),
        ]
        position = 68
        for index, kind, number_format, source in expected:
            self.assertEqual(struct.unpack_from('<HBBB', data, position),
                             (index, kind, len(source), number_format))
            position += 5
            self.assertEqual(data[position:position + len(source)], source)
            position += len(source)
        self.assertEqual(position, len(data))
        canonical = struct.pack('<4sHHHHI26H', b'BSH1', 2, 0, 128, 26, 5, *widths)
        canonical += b''.join(struct.pack('<HBBB', index, kind, len(source), number_format)
                              + source for index, kind, number_format, source in expected)
        self.assertEqual(data, canonical)

    def test_csv_ignores_display_metadata(self):
        path = self.directory / 'formatted.csv'
        self.assertEqual(path.read_bytes(), b'1.235,2.47,0.125\r\n0012,,\r\n')
        with path.open(encoding='ascii', newline='') as source:
            self.assertEqual(list(csv.reader(source, strict=True)),
                             [['1.235', '2.47', '0.125'], ['0012', '', '']])


if __name__ == '__main__':
    unittest.main()
