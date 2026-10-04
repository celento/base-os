"""Exercise Writer through the normal production desktop and disposable disks.

All edits use PS/2 input. DWARF-qualified, bounded guest-memory reads observe the
compiled document model; saved native/RTF files are checked independently on host.
"""
import argparse
import json
import pathlib
import struct
import tempfile
import time
import zlib

from elf_debug import DebugInfo
from init_data import initialize
from video_input_test import Observations, ProductionSession
from volume import DATA_LAYOUT, MAGIC, data_layout, load, resolve

WRITER = 22


def native(text, style=None, paragraphs=None):
    text = text.encode('ascii') if isinstance(text, str) else text
    count = len(text)
    assert count <= 32768
    style = bytes(count + 1) if style is None else bytes(style)
    paragraphs = bytes(count + 1) if paragraphs is None else bytes(paragraphs)
    assert len(style) == len(paragraphs) == count + 1
    return struct.pack('<4sHHII', b'BWR1', 1, 0, count, 0) + text + style + paragraphs


def decode_native(data):
    magic, version, flags, count, reserved = struct.unpack_from('<4sHHII', data)
    assert (magic, version, flags, reserved) == (b'BWR1', 1, 0, 0)
    assert count <= 32768 and len(data) == 18 + count * 3
    return dict(text=data[16:16 + count], style=data[16 + count:17 + count * 2],
                paragraph=data[17 + count * 2:])


def fixture(directory, files, profile="default"):
    layout = data_layout(profile)
    disk = directory / 'writer-data.img'
    assert initialize(disk, profile=profile)
    data = bytearray(disk.read_bytes())
    records = [(0, -1, 1, '', b''), (1, 0, 1, 'Documents', b'')]
    records += [(i + 2, 1, 0, name, content) for i, (name, content) in enumerate(files)]
    payload = bytearray()
    for ident, parent, is_dir, name, content in records:
        payload += struct.pack('<HhBBHI24sI', ident, parent, is_dir, 0, 0,
                               len(content), name.encode(), 0) + content
    header = struct.pack('<6I', MAGIC, layout.version, len(records), len(payload), zlib.crc32(payload), 1)
    header += struct.pack('<I', zlib.crc32(header))
    start = layout.lbas[0] * 512
    data[start:start + 512] = header.ljust(512, b'\0')
    data[start + 512:start + 512 + len(payload)] = payload
    assert len(load(data)[2]) == len(records)
    disk.write_bytes(data)
    return disk


class WriterSession(ProductionSession):
    def __exit__(self, kind, error, traceback):
        if error is not None:
            try:
                self.screenshot('failure.png')
            except Exception:
                pass  # Preserve the original test failure if QEMU has stopped.
        return super().__exit__(kind, error, traceback)

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        time.sleep(.16)


class WriterCheck:
    def __init__(self, session, build):
        self.s = session
        self.o = Observations(session, pathlib.Path(build) / 'kernel.elf')
        debug = DebugInfo(pathlib.Path(build) / 'kernel.elf')
        self.state_address, (self.state_size, self.state_fields) = debug.variable('state', 'src/writer.c')
        self.snapshot_size, self.snapshot_fields = debug.structure('Snapshot', 'src/writer.c')
        self.doc_size, self.doc_fields = debug.structure('WriterDoc', 'src/writer.c')
        arena_size, self.arena_fields = debug.structure('WriterArena', 'src/writer.c')
        assert self.state_size < 4096 and self.doc_size < 100000
        assert arena_size <= session.layout['WRITER_CAPACITY']
        assert self.snapshot_size * 9 <= arena_size

    def state(self):
        raw = self.s.memory(self.state_address, self.state_size)
        return {name: struct.unpack_from('<i', raw, offset)[0]
                for name, offset in self.state_fields.items() if name not in ('title', 'status')}

    def document(self):
        state = self.state()
        index = (state['first'] + state['current']) % 9
        address = self.s.layout['WRITER_BASE'] + self.arena_fields['history'] + index * self.snapshot_size
        raw = self.s.memory(address, self.snapshot_size)
        result = {name: struct.unpack_from('<I', raw, offset)[0]
                  for name, offset in self.snapshot_fields.items() if name != 'doc'}
        doc = raw[self.snapshot_fields['doc']:self.snapshot_fields['doc'] + self.doc_size]
        count = struct.unpack_from('<I', doc, self.doc_fields['length'])[0]
        assert count <= 32768
        result['text'] = doc[self.doc_fields['text']:self.doc_fields['text'] + count]
        for name in ('style', 'paragraph'):
            result[name] = doc[self.doc_fields[name]:self.doc_fields[name] + count + 1]
        return result

    def content(self):
        doc = self.document()
        return {name: doc[name] for name in ('text', 'style', 'paragraph')}

    def owner(self):
        return next(w for w in self.o.windows() if w['open'] and w['kind'] == WRITER)

    def move(self, x, y):
        for _ in range(100):
            mx, my = self.o.integer('mouse_x'), self.o.integer('mouse_y')
            if (mx, my) == (x, y):
                return
            events = [{'type': 'rel', 'data': {'axis': axis, 'value': max(-80, min(80, delta))}}
                      for axis, delta in (('x', x - mx), ('y', y - my)) if delta]
            self.s.command('input-send-event', {'events': events})
            time.sleep(.025)
        raise AssertionError('Mouse did not reach target')

    def click(self, x, y):
        self.move(x, y)
        for down in (True, False):
            self.s.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.10)

    def toolbar(self, x, y):
        window = self.owner()
        self.click(window['x'] + 1 + x, window['y'] + 33 + y)

    def modal(self):
        self.s.wait(lambda: self.o.integer('edit_close_dlg') == 1, 'Writer save guard')
        assert self.o.integer('edit_close_owner') == self.owner()['slot']
        assert self.o.integer('edit_close_focus') == 2

    def choose(self, choice):
        if choice == 'save':
            self.s.key('tab')
        elif choice == 'discard':
            self.s.key('shift-tab')
        else:
            assert choice == 'cancel'
        self.s.key('ret')

    def name(self, value):
        self.s.wait(lambda: self.o.integer('name_dlg') == 1, 'Writer name dialog')
        for _ in range(self.o.integer('name_len')):
            self.s.key('backspace')
        self.s.text(value)
        self.s.key('ret')

    def save_as(self, value):
        self.s.key('ctrl-shift-s')
        self.name(value)
        self.s.wait(lambda: self.o.integer('name_dlg') == 0, 'Writer Save As completed')
        self.s.wait(lambda: self.o.integer('fs_touched') == 0, 'Writer save synchronized')

    def pick(self, ident):
        self.s.wait(lambda: self.o.integer('open_dlg') == 1, 'Writer Open dialog')
        for _ in range(256):
            count = self.o.integer('pick_count')
            ids = struct.unpack('<256i', self.o.read('pick_ids'))[:count]
            selected = self.o.integer('pick_selected')
            if 0 <= selected < count and ids[selected] == ident:
                self.s.key('ret')
                return
            self.s.key('down')
        raise AssertionError('Requested document was not selectable')

    def open_file(self, disk, path):
        nodes = load(disk.read_bytes())[2]
        ident = resolve(nodes, path)
        self.s.key('ctrl-o')
        if path.startswith('/Documents/'):
            self.pick(resolve(nodes, '/Documents'))
        self.pick(ident)


def run(build, profile="default"):
    machine = ["-m", "128M" if profile == "large" else "64M"]
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-input-data-'))
    disk = fixture(work, [('import.txt', b'Windows line one\r\nSecond line\r\n'),
                          ('other.bwr', native('Another saved document.')),
                          ('maximum.txt', b'x' * 32768)], profile)
    evidence = []
    with WriterSession(build, 'writer-input', extra=machine + ['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot()
        check = WriterCheck(session, build)
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.launch('writer')
        assert check.content()['text'] == b''
        session.text('Project notes')
        session.key('ctrl-home'); session.key('ctrl-1'); session.key('ctrl-e')
        session.key('ctrl-end'); session.key('ret'); session.key('ctrl-0'); session.key('ctrl-l')
        session.text('A document with ')
        session.key('ctrl-b'); session.text('bold'); session.key('ctrl-b')
        session.text(' and '); session.key('ctrl-i'); session.text('italic'); session.key('ctrl-i')
        session.text(' text.')
        content = check.content()
        assert content['text'] == b'Project notes\nA document with bold and italic text.'
        assert content['paragraph'][0] == 5 and content['paragraph'][14] == 0
        bold = content['text'].index(b'bold'); italic = content['text'].index(b'italic')
        assert content['style'][bold:bold + 4] == bytes([1]) * 4
        assert content['style'][italic:italic + 6] == bytes([2]) * 6
        evidence.append(str(session.screenshot('writer-styled-document.png')))
        check.save_as('project.bwr')
        nodes = load(disk.read_bytes())[2]
        assert decode_native(nodes[resolve(nodes, '/Documents/project.bwr')]['data']) == content

        # A separate RTF export retains the native binding and dirty state.
        original_file = check.state()['file']
        session.key('ctrl-shift-e'); check.name('project.rtf')
        session.wait(lambda: check.o.integer('name_dlg') == 0, 'RTF export completed')
        assert check.state()['file'] == original_file
        nodes = load(disk.read_bytes())[2]
        rtf = nodes[resolve(nodes, '/Documents/project.rtf')]['data']
        assert rtf.startswith(b'{\\rtf1') and b'\\b' in rtf and b'\\i' in rtf and b'\\qc' in rtf
        (work / 'project.rtf').write_bytes(rtf)

        # Rich copy/undo/redo preserves all character and paragraph attributes.
        session.key('ctrl-a'); session.key('ctrl-c'); session.key('ctrl-end'); session.key('ret'); session.key('ctrl-v')
        doubled = check.content()
        assert doubled['text'] == content['text'] + b'\n' + content['text']
        offset = len(content['text']) + 1
        assert doubled['style'][offset:offset + len(content['text'])] == content['style'][:-1]
        session.key('ctrl-z'); assert check.content()['text'] == content['text'] + b'\n'
        session.key('ctrl-y'); assert check.content() == doubled
        session.key('ctrl-n'); check.modal(); session.key('esc'); assert check.content() == doubled
        session.key('ctrl-w'); check.modal(); check.choose('cancel'); assert check.content() == doubled
        evidence.append(str(session.screenshot('writer-close-cancel-preserved.png')))
        session.key('ctrl-n'); check.modal(); check.choose('discard')
        assert check.content()['text'] == b''

        # Import is a copy; CRLF becomes LF and the original bytes survive.
        check.open_file(disk, '/Documents/import.txt')
        assert check.content()['text'] == b'Windows line one\nSecond line\n'
        assert check.state()['file'] == -1
        session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('imported.bwr')
        session.wait(lambda: not any(w['open'] and w['kind'] == WRITER for w in check.o.windows()), 'saved Writer closed')
        session.launch('writer'); assert check.content()['text'] == b''
        session.text('This discarded draft must not return')
        session.key('ctrl-w'); check.modal(); check.choose('discard')
        session.launch('writer'); assert check.content()['text'] == b''

        # A full valid text document is neither truncated nor extended past cap.
        check.open_file(disk, '/Documents/maximum.txt')
        assert len(check.content()['text']) == 32768
        session.key('ctrl-end'); session.text('z'); assert check.content()['text'] == b'x' * 32768
        session.key('ctrl-home'); session.key('shift-right'); session.key('ctrl-u')
        maximal = check.content(); assert maximal['style'][0] == 4
        check.save_as('maximum.bwr')
        session.key('ctrl-home'); session.key('shift-right'); session.key('ctrl-b')
        recovery = check.content(); caret = check.document()['caret']
        session.wait(lambda: check.o.integer('fs_touched') == 0, 'ordinary disk synchronization')
        # Five-second idle recovery includes the complete maximum-sized styled draft.
        def recovered_on_disk():
            try:
                nodes = load(disk.read_bytes())[2]
                ident = resolve(nodes, '/prefs/writer-draft.bwr')
                return decode_native(nodes[ident]['data']) == recovery
            except ValueError:
                return False  # Normal first autosave has not created its file yet.
        session.wait(recovered_on_disk, 'complete styled recovery draft', 30)
        evidence.append(str(session.screenshot('writer-maximum-document.png')))
        before = check.o.integer('redraw_count')
        session.key('alt-tab'); session.key('alt-tab')
        assert check.o.integer('redraw_count') > before
    with WriterSession(build, 'writer-recovery', extra=machine + ['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = WriterCheck(session, build)
        assert check.content() == recovery and check.document()['caret'] == caret
        evidence.append(str(session.screenshot('writer-recovered-document.png')))
        session.key('ctrl-s')
        session.wait(lambda: check.o.integer('fs_touched') == 0, 'recovered document synchronized')
    nodes = load(disk.read_bytes())[2]
    assert nodes[resolve(nodes, '/Documents/import.txt')]['data'] == b'Windows line one\r\nSecond line\r\n'
    assert decode_native(nodes[resolve(nodes, '/Documents/maximum.bwr')]['data']) == recovery
    result = {'passed': True, 'profile': profile, 'disk': str(disk), 'screenshots': evidence,
              'checks': ['minimum-resolution styling', 'native save/reopen', 'separate RTF export',
                         'rich clipboard', 'undo/redo', 'New/Close cancellation', 'discard stays discarded',
                         'CRLF import copy', '32768-byte exact cap', 'styled maximum-document recovery']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--profile', choices=('default', 'large'), default='default')
    args = parser.parse_args()
    run(args.build.resolve(), args.profile)
