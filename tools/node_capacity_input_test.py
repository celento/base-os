"""Production desktop proof for large folders and backend-specific counters.

python3 tools/node_capacity_input_test.py build

Requires QEMU, binutils, Pillow and Tesseract. Uses fresh disposable images,
ordinary PS/2 keyboard/mouse input, screenshot OCR, and bounded read-only
ELF/DWARF observations. No test kernel, guest-memory writes, or saved disks.
"""
import argparse
import json
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

from PIL import Image
from init_data import initialize
from video_input_test import ProductionSession
from volume import DATA_LAYOUT, MAGIC, load

COUNT = 180
NAMES = [f'document-{i:03}.txt' for i in range(COUNT)]
CONTENT = [f'Document {i:03}\nThis is ordinary document number {i} in a 180-file folder.\n'.encode()
           for i in range(COUNT)]
WK_FILES, WK_EDIT, WK_TERM, WK_SYSMON, WK_PROPERTIES = 4, 5, 11, 18, 19


class CapacitySession(ProductionSession):
    def key(self, key):
        # Keep a full desktop turn between presses even when autosave is busy.
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        self.key_events.append(dict(key=key, hold_ms=35,
                                   elapsed_seconds=round(time.monotonic() - self.started, 3)))
        time.sleep(.18)


class Layouts:
    def __init__(self, elf):
        info = subprocess.check_output(['readelf', '--debug-dump=info', str(elf)], text=True)
        self.entries = {}
        for match in re.finditer(r'<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)(.*?)(?=\n\s*<\d+><|\Z)', info, re.S):
            depth, address, tag, body = match.groups()
            self.entries[int(address, 16)] = (int(depth), tag, body)
        self.symbols = {}
        for line in subprocess.check_output(['nm', '-S', str(elf)], text=True).splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2].lower() in ('b', 'd'):
                self.symbols.setdefault(parts[3], []).append((int(parts[0], 16), int(parts[1], 16)))

    def structure(self, name):
        typedef = next(body for _, tag, body in self.entries.values()
                       if tag == 'DW_TAG_typedef' and re.search(r'DW_AT_name\s*:.*\b' + name + r'\s*\n', body))
        target = int(re.search(r'DW_AT_type\s*: <0x([0-9a-f]+)>', typedef)[1], 16)
        depth, _, body = self.entries[target]
        size = int(re.search(r'DW_AT_byte_size\s*: (0x[0-9a-f]+|\d+)', body)[1], 0)
        members, started = {}, False
        for address, (level, tag, member) in self.entries.items():
            if address == target:
                started = True
                continue
            if not started:
                continue
            if level <= depth:
                break
            if level == depth + 1 and tag == 'DW_TAG_member':
                field = re.search(r'DW_AT_name\s*:(.*)', member)[1].split(':')[-1].strip()
                members[field] = int(re.search(r'DW_AT_data_member_location\s*: (0x[0-9a-f]+|\d+)', member)[1], 0)
        return size, members


def fixture(directory):
    image = directory / 'data.img'
    assert initialize(image)
    raw = bytearray(image.read_bytes())
    records = [(0, -1, 1, '', b''), (1, 0, 1, 'Large', b''),
               (2, 0, 1, 'Documents', b''), (3, 0, 1, 'prefs', b''), (4, 0, 1, 'trash', b'')]
    records += [(i + 5, 1, 0, NAMES[i], CONTENT[i]) for i in range(COUNT)]
    payload = bytearray()
    for ident, parent, directory_flag, name, content in records:
        payload += struct.pack('<HhBBHI24sI', ident, parent, directory_flag, 0, 0,
                               len(content), name.encode(), 1234) + content
    header = struct.pack('<6I', MAGIC, 4, len(records), len(payload), zlib.crc32(payload), 1)
    header += struct.pack('<I', zlib.crc32(header))
    start = DATA_LAYOUT.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    nodes = load(raw)[2]
    assert len(nodes) == COUNT + 5 and nodes[COUNT + 4]['data'] == CONTENT[-1]
    image.write_bytes(raw)
    return image


class Checks:
    def __init__(self, session, layouts):
        self.s, self.layouts = session, layouts
        self.ws, self.wf = layouts.structure('WindowState')
        self.ds, self.df = layouts.structure('Document')
        self.ts, self.tf = layouts.structure('Terminal')
        self.ns, self.nf = layouts.structure('FsNode')
        self.win_size, self.win_fields = layouts.structure('Win')
        assert self.ns == 52 and self.ns * 256 < session.layout['FS_CAPACITY']
        assert self.ws * 8 < session.layout['EDITOR_CAPACITY'] and self.ts * 8 <= 0xc0000
        assert self.tf['lines'] == 0 and self.tf['input'] % 81 == 0
        self.line_limit = self.tf['input'] // 81
        assert self.line_limit >= COUNT, 'Terminal cannot retain a complete large-directory listing'

    def symbol(self, name, size):
        matches = self.layouts.symbols[name]
        assert len(matches) == 1 and size <= matches[0][1] <= 4096
        return self.s.memory(matches[0][0], size)

    def integer(self, name):
        return struct.unpack('<i', self.symbol(name, 4))[0]

    @staticmethod
    def fields(raw, offsets):
        return {name: struct.unpack_from('<i', raw, offset)[0] for name, offset in offsets.items()}

    def windows(self):
        raw = self.symbol('wins', self.win_size * 8)
        return [dict(self.fields(raw[i * self.win_size:(i + 1) * self.win_size], self.win_fields), slot=i)
                for i in range(8)]

    def front(self):
        return max((w for w in self.windows() if w['open'] and not w['min']), key=lambda w: w['z'])

    def launch(self, name, kind):
        self.s.launch(name)
        self.s.wait(lambda: not self.integer('launcher_on') and self.front()['kind'] == kind,
                    'launcher did not open ' + name)
        return self.front()['slot']

    def node_stats(self):
        raw = self.s.memory(self.s.layout['FS_BASE'], self.ns * 256)
        nodes = {}
        for ident in range(256):
            item = raw[ident * self.ns:(ident + 1) * self.ns]
            values = self.fields(item, {k: v for k, v in self.nf.items() if k != 'name'})
            assert values['used'] in (0, 1)
            if values['used']:
                nodes[ident] = dict(values, name=item[:24].split(b'\0')[0].decode('ascii'))
        return nodes

    def folder(self, owner):
        raw = self.s.memory(self.s.layout['EDITOR_BASE'] + owner * self.ws, self.wf['doc'])
        count = struct.unpack_from('<i', raw, self.wf['count'])[0]
        assert 0 <= count <= 256
        result = self.fields(raw, {k: self.wf[k] for k in ('cwd', 'selected', 'first', 'manual_files_scroll')})
        result['ids'] = list(struct.unpack_from('<' + 'i' * count, raw, self.wf['ids']))
        return result

    def document(self, owner):
        raw = self.s.memory(self.s.layout['EDITOR_BASE'] + owner * self.ws + self.wf['doc'], self.ds)
        length = struct.unpack_from('<i', raw, self.df['len'])[0]
        ident = struct.unpack_from('<i', raw, self.df['file'])[0]
        assert 0 <= length < 65536
        return ident, raw[:length]

    def terminal(self, owner):
        raw = self.s.memory(self.s.layout['APPS_BASE'] + 0x300000 + owner * self.ts, self.ts)
        state = self.fields(raw, {k: self.tf[k] for k in ('head', 'count', 'scroll')})
        assert 0 <= state['head'] < self.line_limit and 0 <= state['count'] <= self.line_limit
        state['lines'] = [raw[((state['head'] + n) % self.line_limit) * 81:((state['head'] + n) % self.line_limit + 1) * 81]
                          .split(b'\0')[0].decode('ascii') for n in range(state['count'])]
        state['input'] = raw[self.tf['input']:self.tf['input'] + 81].split(b'\0')[0].decode('ascii')
        return state

    def command(self, text):
        owner = self.front()['slot']
        self.s.text(text)
        self.s.wait(lambda: self.terminal(owner)['input'] == text, 'Terminal did not receive command: ' + text)
        self.s.key('ret')
        self.s.wait(lambda: not self.terminal(owner)['input'], 'Terminal command did not execute: ' + text)
        time.sleep(.12)

    def move(self, x, y):
        deadline = time.monotonic() + 5
        while (self.integer('mouse_x'), self.integer('mouse_y')) != (x, y):
            assert time.monotonic() < deadline, 'Mouse did not reach target'
            events = []
            for axis, current, target in (('x', self.integer('mouse_x'), x), ('y', self.integer('mouse_y'), y)):
                if target != current:
                    events.append({'type': 'rel', 'data': {'axis': axis, 'value': max(-80, min(80, target - current))}})
            self.s.command('input-send-event', {'events': events})
            time.sleep(.025)

    def wheel(self, count):
        for _ in range(count):
            for down in (True, False):
                self.s.command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'down': down, 'button': 'wheel-down'}}]})
            time.sleep(.07)

    def shot_ocr(self, name):
        time.sleep(.2)
        window = self.front()
        screenshot = self.s.screenshot(name + '.png')
        crop = screenshot.with_name(name + '-ocr.png')
        with Image.open(screenshot) as image:
            rect = (window['x'], window['y'], window['x'] + window['w'], window['y'] + window['h'])
            image.crop(rect).resize((window['w'] * 3, window['h'] * 3), Image.Resampling.LANCZOS).save(crop)
        text = subprocess.check_output(['tesseract', str(crop), 'stdout', '--psm', '6'],
                                       stderr=subprocess.DEVNULL, text=True, timeout=20)
        screenshot.with_suffix('.txt').write_text(text)
        return text

    def properties(self, expected_id, limit, label):
        self.s.key('ctrl-i')
        self.s.wait(lambda: self.front()['kind'] == WK_PROPERTIES, 'Properties did not open')
        assert self.integer('properties_id') == expected_id
        deadline = time.monotonic() + 30
        while True:
            before = len(self.node_stats())
            text = self.shot_ocr(label + '-properties')
            after = len(self.node_stats())
            match = re.search(r'Free file/folder slots:\s*(\d+)', text, re.I)
            if before == after and match and int(match[1]) == limit - after:
                break
            assert time.monotonic() < deadline, (limit, after, text)
            time.sleep(.3)
        self.s.key('ctrl-w')
        return dict(nodes=after, limit=limit, free=limit - after, ocr=text)

    def monitor(self, limit, label):
        self.launch('monitor', WK_SYSMON)
        assert self.integer('current_tab') == 0
        # Autosave can add session/draft nodes between frames. Wait for the
        # normal periodic repaint rather than compare one stale screenshot.
        deadline = time.monotonic() + 30
        while True:
            before = len(self.node_stats())
            text = self.shot_ocr(label + '-monitor')
            after = len(self.node_stats())
            match = re.search(r'Files and folders\s+(\d+)\s+of\s+(\d+)', text, re.I)
            if before == after and match and tuple(map(int, match.groups())) == (after, limit):
                break
            assert time.monotonic() < deadline, (after, limit, text)
            time.sleep(.3)
        return dict(nodes=after, limit=limit, ocr=text)


def large_flow(build, layouts, disk):
    with CapacitySession(build, 'node-capacity-desktop', extra=(
            '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback')) as session:
        print(session.directory, flush=True)
        check = Checks(session, layouts)
        try:
            session.boot()
            terminal = check.launch('terminal', WK_TERM)
            check.command('cd /Large')
            check.command('clear')
            check.command('ls')
            session.wait(lambda: NAMES[-1] in check.terminal(terminal)['lines'], 'late Terminal listing entry')
            state = check.terminal(terminal)
            listed = [line for line in state['lines'] if line in NAMES]
            assert listed == NAMES and state['count'] >= COUNT
            (session.directory / 'terminal-listing.txt').write_text('\n'.join(state['lines']) + '\n')
            session.screenshot('terminal-last-entries.png')
            for _ in range(30):
                session.key('pgup')
            assert check.terminal(terminal)['scroll'] > 64
            session.screenshot('terminal-first-entries.png')
            check.command('df')
            session.wait(lambda: any(line.startswith('Free node slots: ') for line in check.terminal(terminal)['lines']), 'Terminal df output')
            nodes = check.node_stats()
            lines = check.terminal(terminal)['lines']
            assert f'Free node slots: {256 - len(nodes)}' in lines
            assert f'Payload capacity: {DATA_LAYOUT.capacity(len(nodes))}' in lines
            session.screenshot('terminal-ide-capacity.png')
            assert [line for line in lines if line in NAMES] == NAMES
            session.key('ctrl-w')

            files = check.launch('files', WK_FILES)
            root_ids = check.folder(files)['ids']
            index = root_ids.index(1)
            for _ in range(index):
                session.key('down')
            session.key('ret')
            session.wait(lambda: check.folder(files)['cwd'] == 1, 'Large folder did not open')
            assert check.folder(files)['ids'] == list(range(5, COUNT + 5))
            session.screenshot('files-first-entries.png')
            w = check.front()
            check.move(w['x'] + w['w'] // 2, w['y'] + w['h'] // 2)
            check.wheel(30)
            session.wait(lambda: check.folder(files)['first'] > 64, 'mouse wheel did not scroll beyond 64 entries')
            session.screenshot('files-middle-entries.png')
            session.key('up')  # Ordinary wrapping selection moves from .. to the last file.
            session.wait(lambda: check.folder(files)['selected'] == COUNT, 'late Files selection')
            assert check.folder(files)['first'] > 64
            session.screenshot('files-last-entry-selected.png')
            selection = check.folder(files)
            selected_id = selection['ids'][selection['selected'] - 1]
            assert check.node_stats()[selected_id]['name'] == NAMES[-1]
            properties = check.properties(COUNT + 4, 256, 'ide')
            session.wait(lambda: check.front()['kind'] == WK_FILES, 'Files focus after Properties')
            session.key('ret')
            session.wait(lambda: check.front()['kind'] == WK_EDIT, 'late document did not open in Editor')
            editor = check.front()['slot']
            assert check.document(editor) == (COUNT + 4, CONTENT[-1])
            text = check.shot_ocr('editor-last-document')
            assert 'Document 179' in text, text
            monitor = check.monitor(256, 'ide')
            report = dict(production_elf=str(build / 'kernel.elf'), document_count=COUNT,
                          terminal_retained=len(listed), terminal_line_limit=check.line_limit,
                          late_file_id=COUNT + 4, late_file=NAMES[-1],
                          properties=properties, monitor=monitor,
                          input_events=session.key_events, captures=session.capture_events)
            (session.directory / 'report.json').write_text(json.dumps(report, indent=2))
            print('IDE production desktop: all 180 ls entries retained, wheel/selection beyond 64, late Editor file, Properties and Monitor counters passed.', flush=True)
            return session.directory
        except Exception:
            session.screenshot('failure.png')
            raise


def floppy_flow(build, layouts):
    with CapacitySession(build, 'node-capacity-floppy') as session:
        print(session.directory, flush=True)
        check = Checks(session, layouts)
        try:
            session.boot()
            terminal = check.launch('terminal', WK_TERM)
            check.command('df')
            session.wait(lambda: any(line.startswith('Free node slots: ') for line in check.terminal(terminal)['lines']), 'Terminal df output')
            nodes = check.node_stats()
            assert f'Free node slots: {64 - len(nodes)}' in check.terminal(terminal)['lines']
            session.screenshot('terminal-floppy-capacity.png')
            session.key('ctrl-w')
            files = check.launch('files', WK_FILES)
            nodes = check.node_stats()
            readme = next(ident for ident, node in nodes.items() if node['name'] == 'readme.txt' and node['parent'] == 0)
            index = check.folder(files)['ids'].index(readme)
            for _ in range(index):
                session.key('down')
            properties = check.properties(readme, 64, 'floppy')
            monitor = check.monitor(64, 'floppy')
            (session.directory / 'report.json').write_text(json.dumps(
                dict(properties=properties, monitor=monitor, input_events=session.key_events), indent=2))
            print('Floppy production desktop: Terminal, Properties and Monitor all report the actual 64-node backend.', flush=True)
            return session.directory
        except Exception:
            session.screenshot('failure.png')
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    assert shutil.which('tesseract'), 'Install Tesseract for screenshot-text assertions'
    build = args.build.resolve()
    layouts = Layouts(build / 'kernel.elf')
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-node-desktop-data-'))
    disk = fixture(directory)
    print(f'Disposable fixture: {directory}', flush=True)
    ide = large_flow(build, layouts, disk)
    persisted = load(disk.read_bytes())[2]
    for i in range(COUNT):
        assert persisted[i + 5]['name'] == NAMES[i] and persisted[i + 5]['data'] == CONTENT[i]
    floppy = floppy_flow(build, layouts)
    (directory / 'evidence.json').write_text(json.dumps(dict(ide=str(ide), floppy=str(floppy)), indent=2))
    print('Both ordinary production desktop capacity flows passed.', flush=True)


if __name__ == '__main__':
    main()
