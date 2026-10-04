"""Production Spreadsheet PS/2 workflows on disposable volumes, no injected state.

Bounded DWARF reads observe exact compiled source/selection; native and CSV output
are independently decoded from disk. All guest mutations use normal input/APIs.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

from elf_debug import DebugInfo
from video_input_test import Observations
from volume import DATA_LAYOUT, commit, load, resolve
from writer_input_test import WriterSession, WriterCheck, fixture

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHEET = 23


def native(records):
    out = bytearray(struct.pack('<4sHHHHI', b'BSH1', 1, 0, 128, 26, len(records)))
    for index, kind, source in sorted(records):
        source = source.encode('ascii') if isinstance(source, str) else source
        assert 0 <= index < 3328 and kind in (1, 2, 3) and len(source) <= 95
        out += struct.pack('<HBB', index, kind, len(source)) + source
    return bytes(out)


def decode(data):
    magic, version, flags, rows, cols, count = struct.unpack_from('<4sHHHHI', data)
    assert (magic, version, flags, rows, cols) == (b'BSH1', 1, 0, 128, 26)
    cursor, previous, records = 16, -1, {}
    for _ in range(count):
        index, kind, size = struct.unpack_from('<HBB', data, cursor)
        assert previous < index < 3328 and kind in (1, 2, 3) and size <= 95
        cursor += 4
        records[index] = (kind, data[cursor:cursor + size])
        cursor += size
        previous = index
    assert cursor == len(data)
    return records


def contents(disk, path):
    nodes = load(disk.read_bytes())[2]
    return nodes[resolve(nodes, path)]['data']


def exists(disk, path):
    try:
        contents(disk, path)
        return True
    except ValueError:
        return False


class SheetSession(WriterSession):
    def text(self, text):
        mapping = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon',
                   '_': 'shift-minus', '\n': 'ret', '=': 'equal', '+': 'shift-equal',
                   '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', ',': 'comma',
                   '"': 'shift-apostrophe', "'": 'apostrophe', '!': 'shift-1'}
        for char in text:
            self.key(mapping.get(char, 'shift-' + char.lower() if char.isupper() else char))


class SheetCheck(WriterCheck):
    def __init__(self, session, build):
        self.s = session
        self.o = Observations(session, pathlib.Path(build) / 'kernel.elf')
        debug = DebugInfo(pathlib.Path(build) / 'kernel.elf')
        self.state_address, (self.state_size, self.state_fields) = debug.variable('state', 'src/sheet.c')
        self.snapshot_size, self.snapshot_fields = debug.structure('Snapshot', 'src/sheet.c')
        self.doc_size, self.doc_fields = debug.structure('SheetDoc', 'src/sheet.c')
        self.cell_size, self.cell_fields = debug.structure('SheetCell', 'src/sheet.c')
        arena_size, self.arena_fields = debug.structure('SpreadsheetArena', 'src/sheet.c')
        assert self.state_size < 4096 and self.cell_size == 104
        assert arena_size == 2824636 <= session.layout['SHEET_CAPACITY']

    def state(self):
        raw = self.s.memory(self.state_address, self.state_size)
        return {name: struct.unpack_from('<i', raw, offset)[0]
                for name, offset in self.state_fields.items()
                if name not in ('title', 'status', 'edit', 'binding')}

    def selected(self):
        state = self.state()
        address = self.s.layout['SHEET_BASE'] + self.arena_fields['history']
        address += ((state['first'] + state['current']) % 5) * self.snapshot_size
        raw = self.s.memory(address + self.snapshot_fields['caret'], 12)
        caret, anchor, revision = struct.unpack('<3I', raw)
        return dict(caret=caret, anchor=anchor, revision=revision, address=address)

    def cell(self, index):
        assert 0 <= index < 3328
        address = self.selected()['address'] + self.snapshot_fields['doc'] + self.doc_fields['cells'] + index * self.cell_size
        raw = self.s.memory(address, self.cell_size)
        length = raw[self.cell_fields['length']]
        assert length <= 95
        return dict(text=raw[self.cell_fields['text']:self.cell_fields['text'] + length],
                    kind=raw[self.cell_fields['kind']], error=raw[self.cell_fields['error']],
                    value=struct.unpack_from('<i', raw, self.cell_fields['value'])[0])

    def expect(self, index, text, kind=None, value=None):
        text = text.encode('ascii') if isinstance(text, str) else text
        def matches():
            cell = self.cell(index)
            return cell['text'] == text and (kind is None or cell['kind'] == kind) and (value is None or cell['value'] == value)
        self.s.wait(matches, f'cell {index} contains {text!r}, got {self.cell(index)}')

    def owner(self):
        return next(w for w in self.o.windows() if w['open'] and w['kind'] == SHEET)

    def jump(self, address):
        self.s.key('ctrl-g'); self.s.key('ctrl-a'); self.s.text(address); self.s.key('ret')
        row = int(address[1:]) - 1; col = ord(address[0].upper()) - ord('A')
        self.s.wait(lambda: self.state()['mode'] == 0 and self.selected()['caret'] == row * 26 + col, 'A1 navigation completed')

    def enter(self, address, source):
        self.jump(address); self.s.text(source); self.s.key('ret')
        self.s.wait(lambda: self.state()['mode'] == 0, 'cell edit committed')

    def modal(self):
        self.s.wait(lambda: self.o.integer('edit_close_dlg') == 1, 'Spreadsheet save guard')
        assert self.o.integer('edit_close_owner') == self.owner()['slot']
        assert self.o.integer('edit_close_focus') == 2

    def save_as(self, value):
        self.s.key('ctrl-shift-s'); self.name(value)
        self.s.wait(lambda: self.o.integer('name_dlg') == 0 and self.o.integer('fs_touched') == 0, 'native save completed')


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-sheet-input-data-'))
    saved = native([(0, 2, '100'), (1, 3, '=A1*2'), (2, 1, 'guarded')])
    changed = native([(0, 2, '200'), (1, 3, '=A1*2'), (2, 1, 'guarded')])
    source = work / 'change.c'
    source.write_text('#include "baseos.h"\nstatic const unsigned char bytes[]={' + ','.join(map(str, changed)) +
                      '};\nint main(void){if(bos_replace_file("/Documents/source.bsh",bytes,sizeof bytes)!=(int)sizeof bytes)return 1;'
                      'return bos_sync()?2:0;}\n')
    app = work / 'change.bex'
    subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'), str(source), str(app)], check=True)
    csv = b'Name,Amount,Note\r\n"comma, quote """,12.500,"line1\nline2"\r\n=2+3,007,last\r\n'
    disk = fixture(work, [('source.bsh', saved), ('import.csv', csv), ('change.bex', app.read_bytes())])
    pictures = []
    with SheetSession(build, 'sheet-input', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.wait(lambda: check.o.integer('fb_w') == 800 and check.o.integer('fb_h') == 600, 'minimum supported display')
        session.launch('spreadsheet'); session.wait(lambda: any(w['open'] and w['kind'] == SHEET for w in check.o.windows()), 'Spreadsheet window opened')
        session.text('cancel this'); session.key('esc'); check.expect(0, b'', 0)
        check.enter('A1', '12.50'); check.enter('B1', '=A1*2'); check.expect(1, '=A1*2', 3, 25000)
        check.jump('A1'); session.key('shift-right'); session.key('ctrl-c'); check.jump('A3'); session.key('ctrl-v')
        check.expect(52, '12.50', 2); check.expect(53, '=A1*2', 3, 25000)
        session.key('ctrl-z'); check.expect(52, b'', 0); session.key('ctrl-y'); check.expect(53, '=A1*2', 3)
        # Editor publishes identical display bytes under a new generation. Paste
        # must import that external text rather than reusing an old formula copy.
        check.jump('B1'); session.key('ctrl-c'); session.launch('editor'); session.text('25')
        session.key('ctrl-a'); session.key('ctrl-c'); session.key('ctrl-w')
        session.wait(lambda: check.o.integer('edit_close_dlg') == 1, 'Editor clipboard fixture guard')
        check.choose('discard'); session.launch('spreadsheet'); check.jump('D1'); session.key('ctrl-v')
        check.expect(3, '25', 2, 25000); session.key('ctrl-z'); check.expect(3, b'', 0)
        # Find/Replace are disabled and cannot type their shortcut letter into a cell.
        before = check.cell(53); session.key('ctrl-f'); session.key('ctrl-h'); assert check.cell(53) == before and check.state()['mode'] == 0
        check.save_as('range.bsh'); expected = decode(contents(disk, '/Documents/range.bsh'))
        session.key('ctrl-shift-e'); check.name('range.csv')
        session.wait(lambda: not check.o.integer('name_dlg'), 'CSV export finished')
        assert contents(disk, '/Documents/range.csv') == b'12.5,25\r\n,\r\n12.5,25\r\n'
        pictures.append(str(session.screenshot('spreadsheet-grid-800x600.png')))
        session.key('ctrl-w'); session.wait(lambda: not any(w['open'] and w['kind'] == SHEET for w in check.o.windows()), 'clean native close')
        session.launch('range.bsh'); check.expect(53, '=A1*2', 3, 25000)
        # Pending cell bytes trigger every replacement guard; Cancel is initial.
        check.jump('C1'); session.text('pending'); session.key('ctrl-n'); check.modal(); check.choose('cancel')
        assert check.state()['mode'] == 1; session.key('ctrl-n'); check.modal(); check.choose('save')
        session.wait(lambda: check.state()['mode'] == 0 and check.cell(0)['kind'] == 0, 'Save-before-New completed')
        assert decode(contents(disk, '/Documents/range.bsh'))[2] == (1, b'pending')
        session.text('New saved'); session.key('ctrl-n'); check.modal(); check.choose('save'); check.name('before-new.bsh')
        session.wait(lambda: check.cell(0)['kind'] == 0 and not check.o.integer('name_dlg'), 'unnamed Save-before-New completed')
        assert decode(contents(disk, '/Documents/before-new.bsh'))[0] == (1, b'New saved')
        session.text('keep me'); check.open_file(disk, '/Documents/source.bsh'); check.modal(); check.choose('cancel')
        assert check.state()['mode'] == 1
        check.open_file(disk, '/Documents/source.bsh'); check.modal(); check.choose('save'); check.name('before-open.bsh'); check.expect(0, '100', 2)
        assert decode(contents(disk, '/Documents/before-open.bsh'))[0] == (1, b'keep me')
        check.jump('C1'); session.text('guarded'); session.key('ctrl-w'); check.modal(); check.choose('cancel'); assert check.state()['mode'] == 1
        session.key('ctrl-w'); check.modal(); check.choose('save')
        session.wait(lambda: not any(w['open'] and w['kind'] == SHEET for w in check.o.windows()), 'Save-before-Close completed')
        session.launch('source.bsh'); check.expect(2, 'guarded', 1)
        check.jump('D1'); session.text('Discard me')
        # Open an import after explicitly discarding the pending work.
        check.open_file(disk, '/Documents/import.csv'); check.modal(); check.choose('discard')
        check.expect(26, 'comma, quote "', 1); check.expect(27, '12.500', 2); check.expect(28, b'line1\nline2', 1)
        check.expect(52, '=2+3', 1); assert check.state()['file'] == -1
        session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('bad.txt')
        session.wait(lambda: check.o.integer('name_failed') == 1, 'invalid native suffix rejected')
        session.key('esc'); assert check.o.integer('edit_close_owner') == -1
        session.key('ctrl-shift-e'); check.name('import-values.csv')
        session.wait(lambda: not check.o.integer('name_dlg'), 'quoted CSV exported')
        assert contents(disk, '/Documents/import-values.csv') == b'Name,Amount,Note\r\n"comma, quote """,12.5,"line1\nline2"\r\n=2+3,7,last\r\n'
        assert contents(disk, '/Documents/import.csv') == csv
        check.save_as('imported.bsh'); session.key('ctrl-w'); session.launch('imported.bsh'); check.expect(28, b'line1\nline2', 1)
        # Resize to exact minimum using observed real window edge drag.
        win = check.owner(); check.move(win['x'] + win['w'] - 2, win['y'] + win['h'] - 2)
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
        session.wait(lambda: check.o.integer('resizing_win') >= 0, 'resize began')
        check.move(win['x'] + 420, win['y'] + 292)
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
        session.wait(lambda: check.owner()['w'] == 422 and check.owner()['h'] == 294, 'minimum window clamps correctly')
        pictures.append(str(session.screenshot('spreadsheet-minimum-window.png')))
        check.open_file(disk, '/Documents/source.bsh'); check.expect(0, '100', 2)
        check.jump('C2'); session.text('Pending recovery')
        def recovered():
            try:
                return decode(contents(disk, '/prefs/sheet-draft.bsh')).get(28) == (1, b'Pending recovery') and len(contents(disk, '/prefs/sheet-binding')) == 52
            except (ValueError, AssertionError):
                return False
        session.wait(recovered, 'pending edit and paired metadata committed', 60)
        assert check.state()['mode'] == 1 and check.cell(28)['kind'] == 0
        baseline = disk.read_bytes()
        session.launch('terminal'); session.text('exec /Documents/change.bex'); session.key('ret')
        session.wait(lambda: contents(disk, '/Documents/source.bsh') == changed, 'normal native app changed the source')
        session.launch('spreadsheet'); session.key('ctrl-s')
        session.wait(lambda: check.o.integer('name_dlg') == 1 and check.o.integer('name_failed') == 1, 'source conflict requires a new name')
        assert contents(disk, '/Documents/source.bsh') == changed
        check.name('source.bsh'); assert check.o.integer('name_failed')
        pictures.append(str(session.screenshot('spreadsheet-source-conflict.png')))
        session.key('esc')
    # Real reboots use stopped disposable disk copies. No live guest bytes change.
    for mode in ('matching', 'changed', 'missing-binding', 'mixed-draft'):
        target = work / (mode + '.img'); target.write_bytes(baseline)
        if mode != 'matching':
            slot, generation, nodes = load(baseline)
            if mode == 'changed': nodes[resolve(nodes, '/Documents/source.bsh')]['data'] = changed
            elif mode == 'missing-binding': del nodes[resolve(nodes, '/prefs/sheet-binding')]
            else:
                draft_id = resolve(nodes, '/prefs/sheet-draft.bsh')
                records = decode(nodes[draft_id]['data']); records[0] = (2, b'300')
                nodes[draft_id]['data'] = native([(i, k, text) for i, (k, text) in records.items()])
            commit(target, baseline, slot, generation, nodes)
        with SheetSession(build, 'sheet-recovery-' + mode, extra=['-drive', f'file={target},format=raw,index=0,if=ide']) as session:
            print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
            check.expect(28, 'Pending recovery', 1)
            before = contents(target, '/Documents/source.bsh')
            session.key('ctrl-s')
            if mode == 'matching':
                session.wait(lambda: decode(contents(target, '/Documents/source.bsh')).get(28) == (1, b'Pending recovery'), 'matched recovery writes original')
                assert not check.o.integer('name_dlg')
            else:
                session.wait(lambda: check.o.integer('name_dlg') == 1, 'unbound recovery requires a new name')
                assert check.state()['file'] == -1 and contents(target, '/Documents/source.bsh') == before
                check.name('recovered.bsh'); session.wait(lambda: not check.o.integer('name_dlg'), 'safe recovery copy saved')
                assert contents(target, '/Documents/source.bsh') == before
            pictures.append(str(session.screenshot('spreadsheet-recovery-' + mode + '.png')))
    unknown = work / 'unknown.img'; unknown.write_bytes(b'Ordinary unknown disk'.ljust(DATA_LAYOUT.sectors * 512, b'\0'))
    digest = hashlib.sha256(unknown.read_bytes()).hexdigest()
    with SheetSession(build, 'sheet-readonly', extra=['-drive', f'file={unknown},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
        session.launch('spreadsheet'); session.text('Do not lose this'); session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('retained.bsh')
        session.wait(lambda: check.o.integer('name_failed') == 1, 'read-only failed sync reported')
        assert check.owner()['open']; check.expect(0, 'Do not lose this', 1)
        session.key('esc'); session.key('ctrl-w'); check.modal(); check.choose('cancel')
        pictures.append(str(session.screenshot('spreadsheet-readonly-save.png')))
    assert hashlib.sha256(unknown.read_bytes()).hexdigest() == digest
    result = dict(passed=True, disk=str(disk), screenshots=pictures,
                  checks=['production PS/2 editing/cancel/formulas/range clipboard/undo', 'exact native and quoted CSV bytes',
                          'New/Open/Close guards and rejected names', '800x600 and minimum window', 'live source conflict',
                          'pending edit paired recovery across four real reboots', 'read-only sync retains work and disk'])
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n'); print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
