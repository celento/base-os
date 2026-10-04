"""Ordinary Spreadsheet UI workflows with actual gfx.c and ASan/UBSan; no QEMU."""
import csv
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SheetUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required for Spreadsheet UI tests')
        cls.temp = tempfile.TemporaryDirectory(prefix='baseos-sheet-ui-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        executable = cls.directory / 'sheet_ui_host'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DSPREADSHEET_HOST_TEST', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests/sheet_ui_host.c'), str(ROOT / 'src/sheet.c'),
                        str(ROOT / 'src/sheet_codec.c'), str(ROOT / 'src/sheet_model.c'),
                        str(ROOT / 'src/decimal.c'), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        env['UBSAN_OPTIONS'] = 'halt_on_error=1'
        # Keep assertion diagnostics visible on failure; normal tests only print markers.
        cls.result = subprocess.run([str(executable), str(cls.directory)],
                                    text=True, stdout=subprocess.PIPE, env=env, check=True)

    def test_sanitized_ordinary_workflows(self):
        self.assertIn('All Spreadsheet UI host functional checks passed.', self.result.stdout)
        for section in ('editing', 'navigation', 'history', 'clipboard', 'files',
                        'bindings', 'recovery', 'rendering'):
            self.assertIn(f'Spreadsheet {section}:', self.result.stdout)

    def test_native_ui_saved_source_kinds_independently(self):
        data = (self.directory / 'ui-native.bsh').read_bytes()
        magic, version, flags, rows, cols, count = struct.unpack_from('<4sHHHHI', data)
        self.assertEqual((magic, version, flags, rows, cols, count),
                         (b'BSH1', 1, 0, 128, 26, 10))
        cursor, previous, records = 16, -1, {}
        for _ in range(count):
            index, kind, length = struct.unpack_from('<HBB', data, cursor)
            cursor += 4
            self.assertLess(previous, index)
            self.assertLess(index, 128 * 26)
            self.assertIn(kind, (1, 2, 3))
            self.assertLessEqual(length, 95)
            records[divmod(index, 26)] = (kind, data[cursor:cursor + length].decode('ascii'))
            cursor += length
            previous = index
        self.assertEqual(cursor, len(data))
        self.assertEqual(records, {
            (0, 0): (1, 'Item'), (0, 1): (1, 'Amount'),
            (1, 0): (1, 'Rent'), (1, 1): (2, '12.5000'),
            (2, 0): (1, 'Total'), (2, 1): (3, '=B2*2'),
            (3, 0): (1, '=2+3'), (3, 1): (1, '007'),
            (4, 0): (1, 'comma, "quoted"'), (4, 1): (1, "'literal"),
        })

    def test_csv_ui_export_calculates_values(self):
        with (self.directory / 'ui-export.csv').open(encoding='ascii', newline='') as source:
            rows = list(csv.reader(source, strict=True))
        self.assertEqual(rows, [
            ['Item', 'Amount'], ['Rent', '12.5'], ['Total', '25'],
            ['=2+3', '007'], ['comma, "quoted"', "'literal"],
        ])


if __name__ == '__main__':
    unittest.main()
