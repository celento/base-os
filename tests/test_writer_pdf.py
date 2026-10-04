"""Freestanding Writer PDF bounds and independent reader/layout verification."""
import importlib.util
import os
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FONTS = ('Times-Roman', 'Times-Bold', 'Times-Italic', 'Times-BoldItalic')


def native_model(path):
    data = path.read_bytes()
    magic, version, flags, length, reserved = struct.unpack_from('<4sHHII', data)
    assert (magic, version, flags, reserved) == (b'BWR1', 1, 0, 0)
    assert len(data) == 18 + 3 * length
    return data[16:16 + length], data[16 + length:17 + 2 * length], data[17 + 2 * length:]


class WriterPdfTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='baseos-writer-pdf-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        executable = cls.directory / 'writer_pdf_test'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests/writer_pdf_test.c'), str(ROOT / 'src/writer_codec.c'),
                        str(ROOT / 'src/writer_pdf.c'), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        cls.host = subprocess.run([str(executable), str(cls.directory)], check=True,
                                  text=True, capture_output=True, env=env)
        cls.fixtures = {name: (int(size), int(pages)) for name, size, pages in re.findall(
            r'^([a-z0-9-]+): (\d+) bytes, (\d+) pages$', cls.host.stdout, re.M)}

    def require(self, *modules):
        for module in modules:
            if importlib.util.find_spec(module) is None:
                self.skipTest(f'{module} is not installed; independent verification not run')

    def reader(self, name):
        self.require('pypdf')
        from pypdf import PdfReader
        return PdfReader(self.directory / f'{name}.pdf', strict=True)

    def test_host_atomic_capacity_bounds_and_cooperative_polling(self):
        self.assertIn('Writer PDF tests passed', self.host.stdout)
        self.assertGreater(self.fixtures['maximum-styles'][0], 2 * 1024 * 1024)
        self.assertLess(self.fixtures['maximum-newlines'][0], 512 * 1024)
        self.assertEqual(self.fixtures['maximum-newlines'][1], 1214)
        self.assertEqual(self.fixtures['forty-lines'][1], 1)
        self.assertEqual(self.fixtures['final-empty-page'][1], 2)

    def test_every_xref_object_offset_and_stream_length(self):
        for name, (size, pages) in self.fixtures.items():
            with self.subTest(name=name):
                data = (self.directory / f'{name}.pdf').read_bytes()
                self.assertEqual(len(data), size)
                marker = re.search(rb'startxref\n(\d+)\n%%EOF\n$', data)
                self.assertIsNotNone(marker)
                xref = int(marker[1])
                header = re.match(rb'xref\n0 (\d+)\n', data[xref:])
                self.assertIsNotNone(header)
                objects = int(header[1])
                self.assertEqual(objects, 7 + 3 * pages)
                start = xref + header.end()
                self.assertEqual(data[start:start + 20], b'0000000000 65535 f \n')
                offsets = {}
                for object_id in range(1, objects):
                    entry = data[start + object_id * 20:start + (object_id + 1) * 20]
                    self.assertRegex(entry, rb'^\d{10} 00000 n \n$')
                    offset = int(entry[:10])
                    offsets[object_id] = offset
                    self.assertTrue(data[offset:].startswith(f'{object_id} 0 obj\n'.encode()))
                for page in range(pages):
                    stream_id = 8 + 3 * page
                    length_id = stream_id + 1
                    stream_start = data.index(b'stream\n', offsets[stream_id]) + len(b'stream\n')
                    length = re.match(rb'\d+ 0 obj\n(\d+)\nendobj', data[offsets[length_id]:])
                    self.assertIsNotNone(length)
                    self.assertEqual(data[stream_start + int(length[1]):][:10], b'endstream\n')

    def test_independent_strict_reader_all_pages_fonts_and_exact_printable_text(self):
        self.require('pypdf')
        from pypdf.generic import ContentStream
        for name, (_, count) in self.fixtures.items():
            with self.subTest(name=name):
                reader = self.reader(name)
                self.assertEqual(len(reader.pages), count)
                text, styles, paragraphs = native_model(self.directory / f'{name}.bwr')
                expected, paragraph = [], paragraphs[0]
                for i, ch in enumerate(text):
                    if i == 0 or text[i - 1] == 10:
                        paragraph = paragraphs[i]
                    if ch >= 32:
                        expected.append((chr(ch), styles[i] & 3, 18 if paragraph & 4 else 12))
                actual = []
                for page in reader.pages:
                    fonts = page['/Resources']['/Font']
                    self.assertEqual(tuple(str(fonts[f'/F{i + 1}']['/BaseFont'])[1:] for i in range(4)), FONTS)
                    for font in fonts.values():
                        self.assertEqual(str(font.get_object()['/Encoding']), '/WinAnsiEncoding')
                    font_index, size = None, None
                    for operands, operator in ContentStream(page['/Contents'], reader).operations:
                        if operator == b'Tf':
                            font_index, size = int(str(operands[0])[2:]) - 1, int(operands[1])
                        if operator == b'Tj':
                            actual.extend((ch, font_index, size) for ch in str(operands[0]))
                self.assertEqual(actual, expected)

    def test_independent_metrics_alignment_style_boundaries_and_underline_geometry(self):
        self.require('pypdf', 'reportlab')
        from pypdf.generic import ContentStream
        from reportlab.pdfbase.pdfmetrics import stringWidth
        for name in ('alignment', 'ascii-styles', 'metrics-grid', 'style-transitions', 'styled-letter',
                     'styled-a4', 'multipage-letter', 'multipage-a4', 'maximum-token', 'indented-long-word'):
            reader = self.reader(name)
            text, styles, paragraphs = native_model(self.directory / f'{name}.bwr')
            # Check each run with independently supplied standard-font widths.
            for page in reader.pages:
                page_width, page_height = float(page.mediabox.width), float(page.mediabox.height)
                rows, underlines = {}, []
                font, size, x, y, pen, thickness = None, None, None, None, None, None
                for operands, operator in ContentStream(page['/Contents'], reader).operations:
                    if operator == b'Tf':
                        font, size = FONTS[int(str(operands[0])[2:]) - 1], float(operands[1])
                    elif operator == b'Tm':
                        self.assertEqual(list(operands[:4]), [1, 0, 0, 1])
                        x, y = float(operands[4]), float(operands[5])
                    elif operator == b'Tj':
                        value = str(operands[0])
                        width = stringWidth(value, font, size)
                        self.assertGreaterEqual(x, 72 - .001)
                        self.assertLessEqual(x + width, page_width - 72 + .001)
                        self.assertGreaterEqual(y - .218 * size, 72 - .001)
                        self.assertLessEqual(y + .935 * size, page_height - 72 + .001)
                        rows.setdefault(y, []).append((x, x + width, value, size))
                    elif operator == b'w':
                        thickness = float(operands[0])
                    elif operator == b'm':
                        pen = tuple(float(v) for v in operands)
                    elif operator == b'l':
                        end = tuple(float(v) for v in operands)
                        underlines.append((pen, end, thickness))
                for baseline, runs in rows.items():
                    for prior, current in zip(runs, runs[1:]):
                        if name not in ('styled-letter', 'styled-a4'):  # Their tab stops are tested separately.
                            self.assertAlmostEqual(prior[1], current[0], delta=.001)
                    if name == 'alignment':
                        row = len(rows) - 1 - sorted(rows).index(baseline)
                        alignment = row % 3
                        if alignment == 0:
                            self.assertAlmostEqual(runs[0][0], 72, delta=.001)
                        elif alignment == 1:
                            self.assertAlmostEqual(runs[0][0] + runs[-1][1], page_width, delta=.001)
                        else:
                            self.assertAlmostEqual(runs[-1][1], page_width - 72, delta=.001)
                for (x0, y0), (x1, y1), thickness in underlines:
                    self.assertEqual(y0, y1)
                    self.assertGreater(x1, x0)
                    size = thickness / .05
                    self.assertIn(round(size), (12, 18))
                    baseline = min(rows, key=lambda y: abs(y - (y0 + size * .1)))
                    self.assertAlmostEqual(y0 + size * .1, baseline, delta=.001)
                    match = [run for run in rows[baseline] if abs(run[0] - x0) < .001]
                    self.assertEqual(len(match), 1)
                    self.assertAlmostEqual(match[0][1], x1, delta=.001)
                if name == 'indented-long-word':
                    # Leading whitespace remains on the hard-wrapped first line.
                    first_line = rows[max(rows)]
                    self.assertTrue(first_line[0][2].startswith('  A'))
                    self.assertGreater(len(first_line[0][2]), 3)

    def test_every_inline_underline_state(self):
        self.require('pypdf')
        from pypdf.generic import ContentStream
        for name in ('ascii-styles', 'metrics-grid', 'style-transitions', 'alignment',
                     'maximum-styles', 'multipage-letter'):
            reader = self.reader(name)
            text, styles, _ = native_model(self.directory / f'{name}.bwr')
            expected = [(chr(ch), style) for ch, style in zip(text, styles) if ch >= 32]
            actual = []
            for page in reader.pages:
                font, run_start = None, 0
                for args, operator in ContentStream(page['/Contents'], reader).operations:
                    if operator == b'Tf':
                        font = int(str(args[0])[2:]) - 1
                    elif operator == b'Tj':
                        run_start = len(actual)
                        actual.extend((ch, font) for ch in str(args[0]))
                    elif operator == b'S':
                        actual[run_start:] = [(ch, style | 4) for ch, style in actual[run_start:]]
            self.assertEqual(actual, expected, name)

    def test_tabs_and_pagination_geometry(self):
        self.require('pypdf')
        from pypdf.generic import ContentStream
        reader = self.reader('tabs')
        rows, x, y = {}, 0, 0
        for args, operator in ContentStream(reader.pages[0]['/Contents'], reader).operations:
            if operator == b'Tm':
                x, y = float(args[4]), float(args[5])
            if operator == b'Tj':
                rows.setdefault(y, []).append(x)
        for (baseline, positions), expected in zip(sorted(rows.items(), reverse=True), (12, 12, 18)):
            self.assertEqual(len(positions), 3)
            self.assertAlmostEqual(positions[1] - positions[0], expected, delta=.001)
            self.assertAlmostEqual(positions[2] - positions[1], expected, delta=.001)
        letter = self.reader('multipage-letter')
        a4 = self.reader('multipage-a4')
        self.assertGreater(len(letter.pages), 1)
        for page in letter.pages:
            self.assertEqual(tuple(float(x) for x in page.mediabox), (0, 0, 612, 792))
        for page in a4.pages:
            self.assertEqual(tuple(float(x) for x in page.mediabox), (0, 0, 595.276, 841.890))

    def test_poppler_extracts_exact_words_without_duplicates(self):
        tool = shutil.which('pdftotext')
        if not tool:
            self.skipTest('Poppler pdftotext is not installed')
        for name in ('styled-letter', 'styled-a4', 'multipage-letter', 'multipage-a4'):
            with self.subTest(name=name):
                result = subprocess.run([tool, '-raw', str(self.directory / f'{name}.pdf'), '-'],
                                        check=True, text=True, capture_output=True)
                self.assertEqual(result.stderr, '')
                expected = (self.directory / f'{name}.txt').read_text()
                self.assertEqual(result.stdout.split(), expected.split())

    def test_independent_mupdf_renders_representative_pages(self):
        self.require('fitz')
        import fitz
        for name, pages in (('styled-letter', (0,)), ('styled-a4', (0,)),
                            ('ascii-styles', (0,)), ('multipage-a4', (0, 1, 5))):
            document = fitz.open(self.directory / f'{name}.pdf')
            for page in pages:
                with self.subTest(name=name, page=page):
                    pixmap = document[page].get_pixmap(matrix=fitz.Matrix(1, 1))
                    self.assertGreater(pixmap.width, 500)
                    self.assertGreater(pixmap.height, 700)
                    self.assertLess(min(pixmap.samples), 255)
                    # Rendering errors and missing fonts are reported by MuPDF.
                    self.assertEqual(fitz.TOOLS.mupdf_warnings(), '')


if __name__ == '__main__':
    unittest.main()
