"""Bounded spreadsheet codecs and independent CSV/native interoperability."""
import csv
import io
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def native_records(data):
    """Independent reader checks canonical sparse BSH1 layout and exact sources."""
    magic, version, flags, rows, cols, count = struct.unpack_from('<4sHHHHI', data)
    assert (magic, version, flags, rows, cols) == (b'BSH1', 1, 0, 128, 26)
    assert count <= 128 * 26
    position, prior, records = 16, -1, {}
    for _ in range(count):
        index, kind, length = struct.unpack_from('<HBB', data, position)
        position += 4
        assert prior < index < 128 * 26 and kind in (1, 2, 3) and length <= 95
        source = data[position:position + length]
        assert len(source) == length
        assert all(32 <= byte <= 126 or byte in (9, 10, 13) for byte in source)
        records[divmod(index, 26)] = kind, source.decode('ascii')
        position += length
        prior = index
    assert position == len(data)
    return records


def csv_rows(path):
    with path.open('r', encoding='ascii', newline='') as source:
        return list(csv.reader(source, strict=True))


class SheetCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required for spreadsheet codec tests')
        cls.temp = tempfile.TemporaryDirectory(prefix='baseos-sheet-codec-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        executable = cls.directory / 'sheet_codec_host'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests/sheet_codec_host.c'),
                        str(ROOT / 'src/sheet_codec.c'),
                        str(ROOT / 'src/sheet_model.c'),
                        str(ROOT / 'src/decimal.c'), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        cls.host_result = subprocess.run([str(executable), str(cls.directory)], check=True,
                                         text=True, capture_output=True, env=env)

    def test_host_codec_bounds_and_atomicity(self):
        self.assertIn('Spreadsheet codec tests passed', self.host_result.stdout)
        self.assertIn('Maximum spreadsheet native: 329488 bytes; CSV: 642432 bytes',
                      self.host_result.stdout)

    def test_native_exact_cell_types_and_sources(self):
        records = native_records((self.directory / 'budget.bsh').read_bytes())
        self.assertEqual(len(records), 22)
        self.assertEqual(records[1, 1], (2, '+001250.5000'))
        self.assertEqual(records[2, 1], (2, '19.995'))
        self.assertEqual(records[4, 3], (3, '=SUM(D2:D4)'))
        self.assertEqual(records[5, 0], (1, 'Notes, "review"\r\nline\tend'))
        self.assertEqual(records[5, 2], (1, '=2+3'))
        self.assertEqual(records[5, 3], (1, "'retained apostrophe"))
        self.assertEqual(records[6, 5], (1, ''))
        self.assertNotIn((6, 4), records)

    def test_native_high_index_little_endian(self):
        data = (self.directory / 'corner.bsh').read_bytes()
        self.assertEqual(data[16:], b'\xff\x0c\x01\x06corner')
        self.assertEqual(native_records(data), {(127, 25): (1, 'corner')})

    def test_csv_python_interoperability_and_calculated_budget(self):
        rows = csv_rows(self.directory / 'budget.csv')
        self.assertEqual(rows, [
            ['Item', 'Cost', 'Quantity', 'Total', '', ''],
            ['Rent', '1250.5', '1', '1250.5', '', ''],
            ['Supplies', '19.995', '3', '59.985', '', ''],
            ['Coffee', '2.5', '2', '5', '', ''],
            ['Budget total', '', '', '1315.485', '', ''],
            ['Notes, "review"\r\nline\tend', '', '=2+3', "'retained apostrophe", '', ''],
            ['', '', '', '', '', ''],
        ])
        canonical = io.StringIO(newline='')
        csv.writer(canonical, lineterminator='\r\n').writerows(rows)
        self.assertEqual((self.directory / 'budget.csv').read_bytes(),
                         canonical.getvalue().encode('ascii'))

    def test_csv_ascii_controls_and_empty_cells(self):
        rows = csv_rows(self.directory / 'ascii.csv')
        self.assertEqual(rows, [
            [''.join(map(chr, range(32, 127))), '\t\r\n'],
            ['', 'tail'],
        ])
        canonical = io.StringIO(newline='')
        csv.writer(canonical, lineterminator='\r\n').writerows(rows)
        self.assertEqual((self.directory / 'ascii.csv').read_bytes(),
                         canonical.getvalue().encode('ascii'))


if __name__ == '__main__':
    unittest.main()
