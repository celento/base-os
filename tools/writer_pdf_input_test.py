"""Writer PDF UI through normal production PS/2 input and disposable disks.

Uses the unmodified packed production boot image. No pause, debugger, memory
observations/writes, or guest-injected calls. All persistent assertions inspect
the stopped data volume; pypdf strictly parses and MuPDF renders saved PDF bytes.
Screenshots cover the 420px toolbar, Letter/A4 controls, errors and close guards.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import tempfile
import time
import zlib

from PIL import Image
from init_data import initialize
from qemu_session import DesktopSession
from volume import MAGIC, data_layout, load, resolve


class PdfSession(DesktopSession):
    def command(self, name, arguments=None):
        assert name in ('qmp_capabilities', 'send-key', 'input-send-event', 'screendump'), name
        return super().command(name, arguments)

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 40})
        time.sleep(.15)

    def text(self, text):
        mapping = {' ': 'spc', '.': 'dot', '\n': 'ret', '-': 'minus',
                   ':': 'shift-semicolon', '/': 'slash'}
        for char in text:
            self.key(mapping.get(char, 'shift-' + char.lower() if char.isupper() else char))

    def memory(self, *args):
        raise AssertionError('Guest memory observations are intentionally unavailable')

    def screenshot(self, name):
        output = self.directory / name
        raw = output.with_suffix('.ppm')
        self.command('screendump', {'filename': str(raw), 'format': 'ppm'})
        with Image.open(raw) as pixels:
            pixels.save(output)
        return output

    def click(self, x, y):
        # Ordinary relative mouse input first reaches a known screen boundary.
        for _ in range(22):
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': axis, 'value': -80}} for axis in ('x', 'y')]})
            time.sleep(.015)
        while x or y:
            dx, dy = min(x, 60), min(y, 60)
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': axis, 'value': value}}
                for axis, value in (('x', dx), ('y', dy)) if value]})
            x -= dx; y -= dy; time.sleep(.025)
        for down in (True, False):
            self.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
            time.sleep(.13)

    def filename(self, name, initial='document.pdf'):
        for _ in initial:
            self.key('backspace')
        self.text(name)

    def export(self, name, paper=0):
        self.key('ctrl-shift-p')
        self.filename(name)
        if paper:
            self.key('tab'); self.key('right'); self.key('spc')
            self.key('tab'); self.key('tab')
        self.key('ret'); time.sleep(.4)


def native(text, style=None, paragraph=None):
    text = text.encode('ascii') if isinstance(text, str) else text
    n = len(text)
    return (struct.pack('<4sHHII', b'BWR1', 1, 0, n, 0) + text +
            (bytes(n + 1) if style is None else bytes(style)) +
            (bytes(n + 1) if paragraph is None else bytes(paragraph)))


def seed(directory):
    text = 'PDF export check\n' + ''.join(f'Line {i:03d}: readable styled pages.\n' for i in range(1, 86))
    style, paragraph = bytearray(len(text) + 1), bytearray(len(text) + 1)
    paragraph[0] = 5  # Centered heading.
    start = 0
    for i, line in enumerate(text.splitlines(keepends=True)):
        if i:
            paragraph[start] = i % 3
            for j in range(start, start + len(line) - 1):
                style[j] = i % 8
        start += len(line)
    original = native(text, style, paragraph)
    dense_text = b'W' * 32768
    dense = native(dense_text, bytes([i & 7 for i in range(32769)]), bytes([4]) + bytes(32768))
    disk = directory / 'writer-pdf-data.img'
    initialize(disk)
    data = bytearray(disk.read_bytes())
    records = [(0, -1, 1, '', b''), (1, 0, 1, 'Documents', b''),
               (2, 1, 0, 'PDF source.bwr', original), (3, 1, 0, 'Dense styles.bwr', dense)]
    payload = b''.join(struct.pack('<HhBBHI24sI', ident, parent, folder, 0, 0,
                                  len(content), name.encode(), 0) + content
                       for ident, parent, folder, name, content in records)
    layout = data_layout('default')
    header = struct.pack('<6I', MAGIC, layout.version, len(records), len(payload), zlib.crc32(payload), 1)
    header += struct.pack('<I', zlib.crc32(header))
    offset = layout.lbas[0] * 512
    data[offset:offset + 512] = header.ljust(512, b'\0')
    data[offset + 512:offset + 512 + len(payload)] = payload
    disk.write_bytes(data)
    return disk, text, original, style, paragraph


def validate_pdf(path, paper, expected_text, render_directory):
    from pypdf import PdfReader
    import fitz
    reader = PdfReader(path, strict=True)
    dimensions = (612, 792) if paper == 0 else (595.276, 841.890)
    assert len(reader.pages) >= 3
    extracted = '\n'.join(page.extract_text() for page in reader.pages)
    for line in expected_text.splitlines():
        if line.strip():
            assert extracted.count(line) == 1, (path, line, extracted.count(line))
    for page in reader.pages:
        assert abs(float(page.mediabox.width) - dimensions[0]) < .001
        assert abs(float(page.mediabox.height) - dimensions[1]) < .001
    rendered = []
    with fitz.open(path) as doc:
        for i in sorted({0, len(doc) - 1}):
            output = render_directory / f'{path.stem}-page-{i + 1}.png'
            pixmap = doc[i].get_pixmap(matrix=fitz.Matrix(1.25, 1.25), alpha=False)
            pixmap.save(output)
            if doc[i].get_text().strip():
                assert len(set(pixmap.samples)) > 10
            else:
                # A final LF may deliberately produce a final empty page.
                assert set(pixmap.samples) == {255}
            rendered.append(str(output))
        warnings = fitz.TOOLS.mupdf_warnings()
        assert not warnings, warnings
    return {'file': str(path), 'bytes': path.stat().st_size, 'pages': len(reader.pages),
            'paper': 'Letter' if paper == 0 else 'A4',
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'renders': rendered}


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-pdf-ui-data-'))
    disk, text, original, style, paragraph = seed(work)
    boot = work / 'production-floppy.img'
    shutil.copyfile(build / 'baseos.img', boot)
    shots = []
    with PdfSession(build, 'writer-pdf-ui', image=boot,
                    extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as guest:
        print('Evidence:', guest.directory, '\nData:', work, flush=True)
        try:
            guest.boot()
            guest.launch('settings'); guest.key('1'); guest.key('ret'); guest.key('ctrl-w')
            guest.launch('PDF source.bwr'); guest.key('alt-left')
            toolbar_shot = guest.screenshot('writer-420px-toolbar.png')
            shots.append(str(toolbar_shot))
            with Image.open(toolbar_shot) as screen:
                dialog_x = (screen.width - 420) // 2
            # Name focus -> Letter -> A4 -> Cancel -> Export; reverse Tab too.
            guest.key('ctrl-shift-p'); guest.filename('a4.pdf')
            guest.key('tab'); guest.key('right'); guest.key('ret')
            shots.append(str(guest.screenshot('pdf-a4-keyboard-focus.png')))
            guest.key('tab'); guest.key('tab'); guest.key('shift-tab'); guest.key('tab'); guest.key('ret')
            time.sleep(.4)
            guest.export('letter.pdf')
            # Minimum-width toolbar: client starts at (3,71) after left snap.
            guest.click(3 + 337, 71 + 47)
            guest.filename('mouse-a4.pdf')
            # Centered dialog at y176. Screen size comes only from its screendump.
            guest.click(dialog_x + 315, 176 + 167)
            shots.append(str(guest.screenshot('pdf-a4-mouse-focus.png')))
            guest.click(dialog_x + 350, 176 + 225); time.sleep(.4)
            # Existing target errors keep the dialog open; Escape preserves work.
            guest.export('letter.pdf')
            shots.append(str(guest.screenshot('pdf-existing-name-protected.png')))
            guest.key('esc')
            guest.key('ctrl-shift-p'); guest.filename('wrong.txt'); guest.key('ret')
            shots.append(str(guest.screenshot('pdf-extension-error.png')))
            guest.key('esc')
            guest.key('ctrl-shift-p'); guest.filename('cancelled.pdf')
            for _ in range(3): guest.key('tab')
            guest.key('ret')
            # RTF's established shortcut and new compact toolbar both work.
            guest.key('ctrl-shift-e'); guest.filename('source.rtf', 'document.rtf'); guest.key('ret')
            guest.click(3 + 291, 71 + 47); guest.filename('toolbar.rtf', 'document.rtf'); guest.key('ret')
            # Export cannot hide dirty work from the ordinary close guard.
            guest.key('ctrl-end'); guest.key('ctrl-b'); guest.text('Unsaved addition.'); guest.key('ctrl-b')
            guest.export('dirty.pdf')
            guest.key('ctrl-w')
            shots.append(str(guest.screenshot('pdf-export-keeps-close-guard.png')))
            guest.key('esc')
            guest.key('ctrl-shift-s'); guest.filename('after.bwr', 'untitled.bwr'); guest.key('ret')
            time.sleep(.4); guest.key('ctrl-w')
            # Arena-overflow rejection uses normal opening and export input.
            guest.launch('Dense styles.bwr'); guest.export('too-large.pdf')
            shots.append(str(guest.screenshot('pdf-export-buffer-limit.png')))
            guest.key('esc'); guest.key('ctrl-w'); time.sleep(1)
        except Exception:
            guest.screenshot('failure.png')
            raise
    # Only a stopped filesystem image is inspected. No live guest memory reads.
    nodes = load(disk.read_bytes())[2]
    by_name = {n['name']: n for n in nodes.values() if n['parent'] == 1}
    assert by_name['PDF source.bwr']['data'] == original
    assert 'cancelled.pdf' not in by_name and 'wrong.txt' not in by_name and 'too-large.pdf' not in by_name
    # Appending bold body text changes only those bytes and preserves all earlier formatting.
    appended = 'Unsaved addition.'
    expected = native(text + appended, bytes(style[:-1]) + bytes([1]) * len(appended) + bytes([0]),
                      bytes(paragraph) + bytes(len(appended)))
    assert by_name['after.bwr']['data'] == expected
    for name in ('source.rtf', 'toolbar.rtf'):
        assert by_name[name]['data'].startswith(b'{\\rtf1')
    assert by_name['source.rtf']['data'] == by_name['toolbar.rtf']['data']
    artifacts = []
    for name, paper in [('letter.pdf', 0), ('a4.pdf', 1), ('mouse-a4.pdf', 1), ('dirty.pdf', 0)]:
        path = work / name
        path.write_bytes(by_name[name]['data'])
        artifacts.append(validate_pdf(path, paper, text + (appended if name == 'dirty.pdf' else ''), work))
    assert by_name['a4.pdf']['data'] == by_name['mouse-a4.pdf']['data']
    result = {'passed': True, 'production_image_sha256': hashlib.sha256(boot.read_bytes()).hexdigest(),
              'disk': str(disk), 'screenshots': shots, 'pdfs': artifacts,
              'checks': ['normal packed boot', '420px toolbar PDF/RTF mouse input',
                         'Letter and A4 keyboard/mouse selection', 'Tab/reverse Tab/Enter/Escape',
                         'existing-name and extension errors', 'Cancel creates no export',
                         'dirty close guard', 'native original and exact Save As preservation',
                         '512KiB arena rejection before file creation',
                         'stopped-volume PDF parsing and independent MuPDF rendering']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
