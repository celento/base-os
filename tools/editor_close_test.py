"""Unsaved Editor close safety through normal production PS/2/QMP input.

Uses disposable boot/data images and read-only, size-checked ELF/DWARF observations.
No kernel fixture, guest-memory writes, persistent images, or fault injection.
Includes a real reboot and a deliberately unrecognized disk's supported read-only
recovery mode to exercise failed saves without corrupting a mounted filesystem.
Requires QEMU, binutils, FFmpeg, and Pillow.
"""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from PIL import Image, ImageChops
from init_data import initialize
from video_fixture import make_fixture
from video_input_test import Observations, ProductionSession
from volume import DATA_LAYOUT, MAGIC, load, resolve

build = pathlib.Path(sys.argv[1]).resolve()
work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-editor-close-data-'))
print(work, flush=True)


def layout_type(name):
    """Get actual compiled member offsets rather than assume Editor arena ABI."""
    info = subprocess.check_output(['readelf', '--debug-dump=info', str(build / 'kernel.elf')], text=True)
    entries = {}
    for match in re.finditer(r'<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)(.*?)(?=\n\s*<\d+><|\Z)', info, re.S):
        depth, address, tag, body = match.groups()
        entries[int(address, 16)] = (int(depth), tag, body)
    typedef = next(body for _, tag, body in entries.values()
                   if tag == 'DW_TAG_typedef' and re.search(r'DW_AT_name\s*:.*\b' + name + r'\s*\n', body))
    target = int(re.search(r'DW_AT_type\s*: <0x([0-9a-f]+)>', typedef)[1], 16)
    depth, _, body = entries[target]
    size = int(re.search(r'DW_AT_byte_size\s*: (0x[0-9a-f]+|\d+)', body)[1], 0)
    members = {}
    started = False
    for address, (entry_depth, tag, member) in entries.items():
        if address == target:
            started = True
            continue
        if not started:
            continue
        if entry_depth <= depth:
            break
        if entry_depth == depth + 1 and tag == 'DW_TAG_member':
            field = re.search(r'DW_AT_name\s*:(.*)', member)[1].split(':')[-1].strip()
            members[field] = int(re.search(r'DW_AT_data_member_location\s*: (0x[0-9a-f]+|\d+)', member)[1], 0)
    return size, members


WINDOW_SIZE, WINDOW_FIELDS = layout_type('WindowState')
DOC_SIZE, DOC_FIELDS = layout_type('Document')
assert DOC_FIELDS['len'] == 65536 and WINDOW_SIZE * 8 < 0x500000 and DOC_SIZE <= 65536 + 256
video = make_fixture(work, width=96, height=64, frames=650, name='close-video', audio=True)
records = [(0, -1, 1, '', b''), (1, 0, 1, 'Documents', b''),
           (2, 1, 0, 'named.txt', b'Original named text'),
           (3, 1, 0, 'empty.txt', b''),
           (4, 1, 0, 'orphan.txt', b'Orphan original'),
           (5, 0, 1, 'Media', b''), (6, 5, 0, video.name, video.read_bytes()),
           (7, 1, 0, 'clear.txt', b'Erase this loaded file')]
disk = work / 'data.img'
initialize(disk)
data = bytearray(disk.read_bytes())
payload = bytearray()
for ident, parent, directory, name, content in records:
    payload += struct.pack('<HhBBHI24sI', ident, parent, directory, 0, 0,
                           len(content), name.encode(), 0) + content
header = struct.pack('<6I', MAGIC, 4, len(records), len(payload), zlib.crc32(payload), 1)
header += struct.pack('<I', zlib.crc32(header))
offset = DATA_LAYOUT.lbas[0] * 512
data[offset:offset + 512] = header.ljust(512, b'\0')
data[offset + 512:offset + 512 + len(payload)] = payload
assert load(data)[2][2]['data'] == b'Original named text'
disk.write_bytes(data)


class CloseSession(ProductionSession):
    def key(self, key):
        # Leave a full desktop turn between physical keys even during saves.
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        time.sleep(.18)

    def boot(self):
        super().boot()
        time.sleep(1)

    def launch(self, name):
        super().launch(name)
        observe = Observations(self, build / 'kernel.elf')
        self.wait(lambda: observe.integer('launcher_on') == 0, 'launcher action completed')
        time.sleep(.15)


class Check:
    def __init__(self, session):
        self.s = session
        self.o = Observations(session, build / 'kernel.elf')

    def integer(self, name):
        return self.o.integer(name)

    def windows(self):
        return self.o.windows()

    def front(self):
        return max((w for w in self.windows() if w['open'] and not w['min']),
                   key=lambda w: w['z'], default={'slot': -1})['slot']

    def doc(self, owner):
        assert 0 <= owner < 8
        raw = self.s.memory(self.s.layout['EDITOR_BASE'] + owner * WINDOW_SIZE + WINDOW_FIELDS['doc'], DOC_SIZE)
        fields = {name: struct.unpack_from('<i', raw, offset)[0]
                  for name, offset in DOC_FIELDS.items() if name != 'buf'}
        assert 0 <= fields['len'] < 65536
        fields['text'] = raw[:fields['len']]
        return fields

    def preserved(self, owner):
        d = self.doc(owner)
        return {name: d[name] for name in ('text', 'caret', 'file', 'identity', 'saved_ok', 'sel_a', 'sel_b')}

    def move(self, x, y):
        for _ in range(80):
            mx, my = self.integer('mouse_x'), self.integer('mouse_y')
            if (mx, my) == (x, y):
                return
            events = []
            for axis, delta in (('x', x - mx), ('y', y - my)):
                if delta:
                    events.append({'type': 'rel', 'data': {'axis': axis, 'value': max(-80, min(80, delta))}})
            self.s.command('input-send-event', {'events': events})
            time.sleep(.025)
        raise AssertionError('Mouse did not reach target')

    def click(self, x, y, button='left'):
        self.move(x, y)
        for down in (True, False):
            self.s.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': button}}]})
            time.sleep(.09)

    def focus(self, owner):
        if self.front() == owner:
            return
        # Close can return before the next taskbar layout has been presented.
        self.s.wait(lambda: self.integer('tb_n') == sum(w['open'] for w in self.windows()),
                    'taskbar relayout after close')
        time.sleep(.2)
        if self.front() == owner:
            return
        count = self.integer('tb_n')
        ids = struct.unpack('<8i', self.o.read('tb_id'))
        index = ids[:count].index(owner)
        xs, widths = (struct.unpack('<8i', self.o.read(name)) for name in ('tb_x', 'tb_w'))
        self.click(xs[index] + widths[index] // 2, self.integer('fb_h') - 22)
        self.s.wait(lambda: self.front() == owner, 'focus owner')

    def modal(self, owner):
        self.s.wait(lambda: self.integer('edit_close_dlg') == 1, 'close confirmation missing')
        assert self.integer('edit_close_owner') == owner and self.integer('edit_close_focus') == 2
        assert self.windows()[owner]['open']

    def choose(self, choice, mouse=False):
        if mouse:
            x, y = (self.integer('fb_w') - 460) // 2, (self.integer('fb_h') - 184) // 2
            self.click(x + 174 + 106 * ('save', 'discard', 'cancel').index(choice), y + 146)
            return
        # Cancel is the initial focus. Tab -> Save; Shift+Tab -> Discard.
        if choice == 'save':
            self.s.key('tab')
        elif choice == 'discard':
            self.s.key('shift-tab')
        self.s.key('ret')

    def name(self, value):
        self.s.wait(lambda: self.integer('name_dlg') == 1, 'Save As missing')
        # Existing name dialog uses append/backspace rather than select-all.
        for _ in range(self.integer('name_len')):
            self.s.key('backspace')
        self.s.text(value)

    def close(self, owner, choice='discard'):
        self.s.key('ctrl-w')
        self.modal(owner)
        self.choose(choice)


expected = {'/close-new.txt': b'Second independent document',
            '/Documents/named.txt': b'Original named text plus',
            '/Documents/clear.txt': b'', '/Documents/reused.txt': b'',
            '/Documents/orphan-rescued.txt': b'Orphan original changed'}
with CloseSession(build, 'editor-close', extra=('-drive', f'file={disk},format=raw,index=0,if=ide',
                                                    '-audiodev', 'driver=none,id=silent',
                                                    '-device', 'sb16,audiodev=silent')) as session:
    print(session.directory, flush=True)
    check = Check(session)
    try:
        session.boot()
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.wait(lambda: check.integer('fb_w') == 800 and check.integer('fb_h') == 600,
                     'minimum desktop resolution')
        session.launch('editor')
        owner = check.front()
        assert check.doc(owner)['saved_ok'] == 1
        session.key('ctrl-w')
        assert not check.windows()[owner]['open'] and not check.integer('edit_close_dlg')

        session.launch('editor')
        owner = check.front()
        session.text('Keep this selected text')
        session.key('ctrl-home'); session.key('shift-right'); session.key('shift-right')
        original = check.preserved(owner)
        session.key('ctrl-w'); check.modal(owner)
        session.screenshot('unsaved-close.png')
        check.choose('cancel')
        assert check.preserved(owner) == original and not check.integer('edit_close_dlg')
        for _ in range(2):
            session.key('ctrl-w'); check.modal(owner)
            for key in ('ctrl-w', 'ctrl-n', 'alt-tab', 'ctrl-s', 'x'):
                session.key(key)
            check.click(30, 15)
            check.click(30, 15, button='right')
            session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'wheel-down'}},
                                                           {'type': 'btn', 'data': {'down': False, 'button': 'wheel-down'}}]})
            assert check.preserved(owner) == original and check.front() == owner
            assert sum(w['open'] for w in check.windows()) == 1
            session.key('esc')
        window = check.windows()[owner]
        check.click(window['x'] + 18, window['y'] + 17); check.modal(owner)
        x, y = (check.integer('fb_w') - 460) // 2, (check.integer('fb_h') - 184) // 2
        check.click(x + 386, y + 146)
        assert not check.integer('edit_close_dlg') and check.preserved(owner) == original
        session.key('f10'); session.key('down'); session.key('down'); session.key('ret')
        check.modal(owner); check.choose('discard')
        assert not check.windows()[owner]['open']
        print('Empty close, conservative Cancel, all close routes, selection and modal input isolation passed.', flush=True)

        session.launch('editor'); first = check.front(); session.text('First independent document')
        first_before = check.preserved(first)
        session.key('ctrl-n'); second = check.front(); session.text(expected['/close-new.txt'].decode())
        second_before = check.preserved(second)
        check.close(second, 'save'); session.key('esc')
        assert check.preserved(first) == first_before and check.preserved(second) == second_before
        assert check.integer('edit_close_owner') == -1
        check.close(second, 'save'); check.name('Documents'); session.key('ret')
        assert check.integer('name_dlg') == 1 and check.integer('name_failed') == 1
        assert check.preserved(second) == second_before
        session.screenshot('save-as-failed.png')
        session.key('tab'); session.key('ret')
        assert not check.integer('name_dlg') and check.windows()[second]['open']
        check.close(second, 'save'); check.name('close-new.txt'); session.key('ret')
        session.wait(lambda: not check.windows()[second]['open'], 'new document close after save')
        assert check.preserved(first) == first_before
        session.key('ctrl-w'); check.modal(first); check.choose('discard', mouse=True)
        assert not check.windows()[first]['open']

        session.launch('named.txt'); owner = check.front(); session.text(' plus')
        session.key('ctrl-w'); check.modal(owner); check.choose('save', mouse=True)
        session.wait(lambda: not check.windows()[owner]['open'], 'named close after save')
        # Reopening from the filesystem, not just observing the old editor buffer.
        session.launch('named.txt'); owner = check.front()
        assert check.doc(owner)['text'] == b'Original named text plus'
        session.key('ctrl-w')
        assert not check.windows()[owner]['open'] and not check.integer('edit_close_dlg')
        session.launch('clear.txt'); owner = check.front()
        session.key('ctrl-a'); session.key('delete')
        assert check.doc(owner)['text'] == b'' and not check.doc(owner)['saved_ok']
        session.key('ctrl-w'); check.modal(owner); session.key('esc')
        assert check.windows()[owner]['open'] and check.doc(owner)['text'] == b''
        check.close(owner, 'save')
        session.launch('empty.txt'); owner = check.front(); session.key('ctrl-w')
        assert not check.windows()[owner]['open'] and not check.integer('edit_close_dlg')

        session.launch('orphan.txt'); orphan = check.front(); old_id = check.doc(orphan)['file']
        session.text(' changed')
        session.launch('terminal'); terminal = check.front()
        session.text('rm /Documents/orphan.txt'); session.key('ret')
        session.text('touch /Documents/reused.txt'); session.key('ret')
        # Normal terminal delete/create reuses the freed ID; no guest state injection.
        fs_raw = session.memory(session.layout['FS_BASE'] + old_id * 52, 52)
        assert fs_raw[:24].split(b'\0')[0] == b'reused.txt', 'expected reused file ID'
        check.focus(orphan); check.close(orphan, 'save')
        assert check.integer('name_dlg') == 1 and check.doc(orphan)['file'] == -1
        session.key('esc')
        assert check.doc(orphan)['text'] == expected['/Documents/orphan-rescued.txt']
        check.close(orphan, 'save'); check.name('orphan-rescued.txt'); session.key('ret')
        session.wait(lambda: not check.windows()[orphan]['open'], 'orphan Save As close')
        check.focus(terminal)
        session.text('start /Programs/counter.bex'); session.key('ret')
        session.launch('editor'); owner = check.front(); session.text('Monitor must protect me')
        original = check.preserved(owner)
        session.launch('monitor'); monitor = check.front()
        client = struct.unpack('<4i', check.o.read('drawn_client'))
        mx, my, mw, _ = client
        tab_width = (mw - 44) // 3
        check.click(mx + 16 + tab_width + 6 + tab_width // 2, my + 26)
        session.wait(lambda: check.integer('current_tab') == 1, 'Monitor Windows tab')
        ids = struct.unpack('<8i', check.o.read('drawn_windows'))
        row = ids[:check.integer('drawn_win_n')].index(owner)
        check.click(mx + mw - 51, my + 74 + row * 38 + 16)
        check.modal(owner); session.key('esc')
        assert check.preserved(owner) == original and check.windows()[monitor]['open']
        session.screenshot('monitor-editor-protected.png')
        print('Save As cancellation/failure, independent documents, named/emptied save, stale ID and Monitor Close passed.', flush=True)

        # A modal above a playing movie must survive full redraws. Native tasks
        # and audio/video still advance; user input cannot reach them through it.
        session.launch(video.name)
        session.wait(lambda: check.o.video()['state'] == 2, 'video playing')
        player = check.front()
        check.focus(owner)
        session.key('ctrl-w'); check.modal(owner)
        a = check.o.video(); audio = check.o.audio(); ticks = check.integer('frame_count')
        before = session.screenshot('media-close-before.png')
        time.sleep(.65)
        after = session.screenshot('media-close-after.png')
        b = check.o.video()
        assert b['displayed_frames'] > a['displayed_frames'] and b['state'] == 2
        assert check.o.audio()['played_frames'] > audio['played_frames']
        assert check.integer('frame_count') > ticks
        with Image.open(before) as aa, Image.open(after) as bb:
            crop = (x + 10, y + 10, x + 450, y + 174)
            assert ImageChops.difference(aa.convert('RGB').crop(crop), bb.convert('RGB').crop(crop)).getbbox() is None, 'video overwrote modal'
        assert check.preserved(owner) == original
        session.key('esc'); check.focus(player); session.key('ctrl-w')
        # Returning to the native counter proves it survived the modal interval.
        check.focus(terminal); session.key('s')
        marker = f'Saved /Documents/counter-{terminal + 1}.txt'.encode()
        session.wait(lambda: marker in session.memory(session.layout['APPS_BASE'] + 0x300000, 0xc0000), 'native task still responsive')
        session.key('ctrl-w')
        session.wait(lambda: not check.windows()[terminal]['open'], 'native terminal close completed')
        check.focus(owner); check.close(owner)
        check.focus(monitor); session.key('ctrl-w')
        print('Playing media preserves modal pixels; video/audio, desktop ticks and native task continue.', flush=True)
    except Exception:
        session.screenshot('failure.png')
        print(session.log.read_text(), flush=True)
        raise

# Inspect committed snapshots with QEMU stopped, then actually boot them again.
_, _, nodes = load(disk.read_bytes())
for path, content in expected.items():
    assert nodes[resolve(nodes, path)]['data'] == content, path
with CloseSession(build, 'editor-close-reboot', extra=('-drive', f'file={disk},format=raw,index=0,if=ide')) as session:
    print(session.directory, flush=True)
    session.boot(); check = Check(session)
    for path, content in expected.items():
        session.launch(path.rsplit('/', 1)[-1])
        owner = check.front()
        assert check.doc(owner)['text'] == content, ('reboot contents', path)
        session.key('ctrl-w')
        if check.integer('edit_close_dlg'):
            check.choose('discard')
    print('Actual reboot reopened exact saved new, named-empty, rescued and unrelated reused file bytes.', flush=True)

# Supported recovery mode: an unknown IDE disk is exposed read-only, not formatted.
unknown = work / 'unknown.img'
unknown.write_bytes(b'\0' * (16 * 1024 * 1024))
with CloseSession(build, 'editor-close-readonly', extra=('-drive', f'file={unknown},format=raw,index=0,if=ide')) as session:
    print(session.directory, flush=True)
    session.boot(); check = Check(session)
    session.launch('editor'); owner = check.front(); session.text('Never discard on failed disk save')
    check.close(owner, 'save'); check.name('unsynced.txt'); session.key('ret')
    assert check.integer('name_dlg') and check.integer('name_failed')
    assert check.windows()[owner]['open'] and not check.doc(owner)['saved_ok']
    session.screenshot('disk-save-failed.png')
    session.key('esc')
    check.close(owner, 'save')
    assert check.integer('edit_close_dlg') and check.integer('edit_close_failed')
    assert check.windows()[owner]['open'] and not check.doc(owner)['saved_ok']
    session.screenshot('named-disk-save-failed.png')
    session.key('esc')
    check.close(owner)
assert unknown.read_bytes() == b'\0' * (16 * 1024 * 1024), 'recovery mode altered unknown disk'
print('EDITOR-CLOSE-PASS: production input, modal isolation, honest failed saves and reboot persistence.', flush=True)
