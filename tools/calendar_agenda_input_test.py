"""Production Calendar agenda PS/2 workflows on fresh disposable data profiles.

Only ordinary keyboard/mouse actions mutate the guest. Source-qualified DWARF
and size-checked QMP pmemsave reads observe Calendar state; there are no debugger
channels, injected calls, guest-memory writes, pauses, or fault probes. Complete
agenda files and unrelated fixture bytes are decoded only after QEMU stops.

Usage: python3 tools/calendar_agenda_input_test.py BUILD [--profile large]
       python3 tools/calendar_agenda_input_test.py --self-test
Never opens a build's saved boot/data image. The harness creates its own boot
image from boot.bin/kernel.bin and a new marked data disk for every scenario.
"""
import argparse
import calendar
import datetime
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import tempfile
import time
import zlib

from elf_debug import DebugInfo
from file_clipboard_input_test import FilesCheck
from init_data import initialize
from video_input_test import Observations, ProductionSession
from volume import data_layout, encode_snapshot, load, resolve
from writer_input_test import WriterCheck

ROOT = pathlib.Path(__file__).resolve().parents[1]
CALENDAR = 14
MAX_ITEMS, HEADER, RECORD, DRAFT = 128, 32, 108, 113
CA_SAVED, CA_PATH_FAILED, CA_CONFLICT, CA_SYNC_FAILED = 3, 4, 7, 8
GUARD = bytes(range(256)) * 8


def padded_text(raw):
    """Strict independently decoded printable ASCII with canonical zero fill."""
    assert b'\0' in raw, 'Missing string terminator'
    value, padding = raw.split(b'\0', 1)
    assert not any(padding), 'Nonzero string padding'
    assert all(32 <= byte <= 126 for byte in value), 'Nonprintable text'
    return value.decode('ascii')


def decode_agenda(data):
    """Independent BCA1 version-1 decoder, including all header and CRC fields."""
    assert HEADER + DRAFT <= len(data) <= HEADER + MAX_ITEMS * RECORD + DRAFT
    magic, version, header, count, active, field, next_id, edit_id, size, crc, reserved = \
        struct.unpack_from('<4sHHHBBIIIII', data)
    assert (magic, version, header, reserved) == (b'BCA1', 1, HEADER, 0)
    assert size == len(data) == HEADER + count * RECORD + DRAFT and count <= MAX_ITEMS
    assert active in (0, 1) and field in (0, 1, 2)
    assert crc == zlib.crc32(data[:24] + bytes(4) + data[28:]), 'BCA1 CRC mismatch'
    items, ids, previous = [], set(), None
    for index in range(count):
        offset = HEADER + index * RECORD
        ident, year, month, day, minute, length, zero = struct.unpack_from('<IHBBHBB', data, offset)
        title = padded_text(data[offset + 12:offset + RECORD])
        assert ident and ident not in ids and (not next_id or ident < next_id)
        assert not zero and len(title) == length <= 95 and title.strip(' ')
        assert 1900 <= year <= 9999
        datetime.date(year, month, day)
        assert minute == 65535 or 0 <= minute <= 1439
        minute = -1 if minute == 65535 else minute
        order = (year, month, day, minute, ident)
        assert previous is None or previous < order, 'Agenda records are not strictly sorted'
        previous = order
        ids.add(ident)
        items.append(dict(id=ident, year=year, month=month, day=day, minute=minute, title=title))
    offset = HEADER + count * RECORD
    draft = dict(active=active, field=field, edit_id=edit_id,
                 date=padded_text(data[offset:offset + 11]),
                 time=padded_text(data[offset + 11:offset + 17]),
                 title=padded_text(data[offset + 17:offset + DRAFT]))
    if not active:
        assert not edit_id and not field and not any(data[offset:])
    elif edit_id:
        assert edit_id in ids
    return dict(version=version, next_id=next_id, items=items, draft=draft)


def make_disk(directory, profile, collision=False):
    disk = directory / ('collision-data.img' if collision else 'agenda-data.img')
    assert initialize(disk, profile=profile), 'Refusing an existing test disk'
    nodes = {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0),
        2: dict(parent=1, name='guard.bin', directory=0, app=0, data=GUARD, modified=123456),
    }
    if collision:
        nodes.update({
            3: dict(parent=0, name='prefs', directory=1, app=0, data=b'', modified=0),
            4: dict(parent=3, name='calendar.v1', directory=1, app=0, data=b'', modified=0),
            5: dict(parent=4, name='keep.txt', directory=0, app=0,
                    data=b'Keep the occupied Calendar path intact.\n', modified=234567),
        })
    layout = data_layout(profile)
    header, payload = encode_snapshot(nodes, layout, 1)
    data = bytearray(disk.read_bytes())
    offset = layout.lbas[0] * 512
    data[offset:offset + 512] = header.ljust(512, b'\0')
    data[offset + 512:offset + 512 + len(payload)] = payload
    assert load(data)[2] == nodes
    disk.write_bytes(data)
    return disk, nodes


class CalendarSession(ProductionSession):
    def command(self, name, arguments=None):
        assert name in ('qmp_capabilities', 'send-key', 'input-send-event', 'screendump', 'pmemsave'), name
        return super().command(name, arguments)

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part} for part in key.split('-')],
                                  'hold-time': 35})
        self.key_events.append(dict(key=key, hold_ms=35,
                                    elapsed_seconds=round(time.monotonic() - self.started, 3)))
        time.sleep(.16)

    def shutdown_keys(self):
        for key in ('f10', 'right', 'right', 'up', 'ret'):
            self.key(key)

    def shutdown(self):
        self.shutdown_keys()
        assert self.process.wait(timeout=90) == 0, 'Safe System Shutdown did not finish'

    def __exit__(self, kind, error, traceback):
        if error is not None and self.process.poll() is None:
            try:
                self.screenshot('failure.png')
            except Exception:
                pass
        try:
            return super().__exit__(kind, error, traceback)
        finally:
            (self.directory / 'input-events.json').write_text(json.dumps(self.key_events, indent=2) + '\n')
            (self.directory / 'captures.json').write_text(json.dumps(self.capture_events, indent=2) + '\n')


class CalendarCheck:
    # Reuse production relative PS/2 helpers, without Writer's state reader.
    move, click = WriterCheck.move, WriterCheck.click

    def __init__(self, session, build):
        self.s = session
        self.o = Observations(session, pathlib.Path(build) / 'kernel.elf')
        self.debug = DebugInfo(pathlib.Path(build) / 'kernel.elf')
        self.item_size, self.item_fields = self.debug.structure('CalAppointment', 'src/calendar_agenda.c')
        self.draft_address, (self.draft_size, self.draft_fields) = self.debug.variable('ca_draft', 'src/calendar_agenda.c')
        assert self.item_size == 116 and self.draft_size == 128
        self.items_address = self.address('ca_items', 'src/calendar_agenda.c', MAX_ITEMS * self.item_size)
        self.addresses = {}
        for name in ('ca_count', 'ca_state', 'ca_pending', 'ca_next_id'):
            self.addresses[name] = self.address(name, 'src/calendar_agenda.c', 4)
        for name in ('view_year', 'view_month', 'view_day', 'selected', 'first_row', 'confirm', 'entry_focus'):
            self.addresses[name] = self.address(name, 'src/calendar.c', 4)

    def address(self, name, unit, size):
        _, entry = self.debug.named(name, ('DW_TAG_variable',), unit)
        match = re.search(r'DW_OP_addr: ([0-9a-f]+)', entry['body'])
        assert match, 'Missing fixed address for ' + name
        address = int(match[1], 16)
        assert (address, size) in self.o.symbols[name], 'Unexpected symbol size for ' + name
        return address

    def integer(self, name):
        return struct.unpack('<I', self.s.memory(self.addresses[name], 4))[0]

    def draft(self):
        raw = self.s.memory(self.draft_address, self.draft_size)
        result = {key: struct.unpack_from('<I', raw, self.draft_fields[key])[0]
                  for key in ('active', 'field', 'edit_id')}
        for key, size in (('date', 11), ('time', 6), ('title', 96)):
            start = self.draft_fields[key]
            result[key] = padded_text(raw[start:start + size])
        return result

    def items(self):
        count = self.integer('ca_count')
        assert 0 <= count <= MAX_ITEMS
        result = []
        for index in range(count):
            raw = self.s.memory(self.items_address + index * self.item_size, self.item_size)
            item = {key: struct.unpack_from('<i' if key != 'id' else '<I', raw, self.item_fields[key])[0]
                    for key in ('id', 'year', 'month', 'day', 'minute')}
            start = self.item_fields['title']
            item['title'] = padded_text(raw[start:start + 96])
            result.append(item)
        return result

    def snapshot(self):
        return dict(version=1, next_id=self.integer('ca_next_id'), items=self.items(), draft=self.draft())

    def owner(self):
        return next((w for w in self.o.windows() if w['open'] and w['kind'] == CALENDAR), None)

    def open(self):
        self.s.launch('calendar')
        self.s.wait(lambda: self.owner() is not None, 'Calendar opened')

    def close(self, key='ctrl-w'):
        self.s.key(key)
        self.s.wait(lambda: self.owner() is None, 'Calendar closed with draft intact')

    def body(self, x, y):
        win = self.owner()
        assert win and win['w'] >= 620 and win['h'] >= 506
        self.click(win['x'] + x, win['y'] + 33 + y)

    def date(self):
        return tuple(self.integer('view_' + key) for key in ('year', 'month', 'day'))

    def choose_day(self, day):
        year, month, _ = self.date()
        first = (calendar.weekday(year, month, 1) + 1) % 7
        index = first + day - 1
        width = ((self.owner()['w'] // 2 - 6) - 32) // 7
        self.body(16 + index % 7 * width + width // 2, 86 + index // 7 * 28 + 14)
        self.s.wait(lambda: self.date() == (year, month, day), 'Month grid selected actual day')

    def field(self, index, value):
        assert index in (0, 1, 2)
        self.body(*((55, 358), (218, 358), (116, 400))[index])
        self.s.key('ctrl-a')
        if value:
            self.s.text(value)
        else:
            self.s.key('backspace')
        self.s.wait(lambda: self.draft()[('date', 'time', 'title')[index]] == value,
                    'Exact draft field replacement')

    def add(self, title, when=''):
        before = self.integer('ca_count')
        self.s.key('n')
        self.s.wait(lambda: self.draft()['active'] == 1 and self.draft()['field'] == 2, 'New draft')
        self.s.text(title)
        if when:
            self.s.key('shift-tab')
            self.s.text(when)
        self.s.key('ret')
        self.s.wait(lambda: not self.draft()['active'] and self.integer('ca_count') == before + 1,
                    'Appointment accepted')
        return self.integer('selected')

    def select_row(self, row):
        self.body(self.owner()['w'] // 2 + 90, 70 + row * 23 + 10)

    def save(self):
        self.s.key('ctrl-s')
        self.s.wait(lambda: self.integer('ca_state') == CA_SAVED and not self.integer('ca_pending'),
                    'Calendar appointments and draft durable', 45)

    def cancel(self):
        self.body(self.owner()['w'] - 50, 306)
        self.s.wait(lambda: self.integer('confirm') == 2, 'Discard-entry guard shown')


def expect_stopped(disk, expected, original_nodes):
    # Caller has exited its session: no observation races with a disk writer.
    nodes = load(disk.read_bytes())[2]
    actual = decode_agenda(nodes[resolve(nodes, '/prefs/calendar.v1')]['data'])
    assert actual == expected, (actual, expected)
    assert nodes[resolve(nodes, '/Documents/guard.bin')] == original_nodes[2]
    return actual


def run(build, profile='default', scenario='all'):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-calendar-agenda-' + profile + '-'))
    print(work, flush=True)
    pictures, sessions, checks = [], [], []
    machine = ['-m', '128M' if profile == 'large' else '64M', '-nic', 'none',
               '-rtc', 'base=2028-01-31T12:00:00,clock=vm']

    def session_for(disk, label):
        session = CalendarSession(build, 'calendar-' + label,
                                  extra=machine + ['-drive', f'file={disk},format=raw,index=0,if=ide'])
        sessions.append(str(session.directory))
        print(label, session.directory, flush=True)
        return session

    if scenario in ('all', 'main'):
        disk, original_nodes = make_disk(work, profile)
        with session_for(disk, 'workflows') as session:
            session.boot()
            check = CalendarCheck(session, build)
            session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
            session.wait(lambda: check.o.integer('fb_w') == 800 and check.o.integer('fb_h') == 600,
                         'Minimum supported 800x600 display')
            check.open()
            assert check.date() == (2028, 1, 31)
            session.key('pgdn'); assert check.date() == (2028, 2, 29)
            session.key('pgdn'); assert check.date() == (2028, 3, 29)
            session.key('pgup'); session.key('left'); assert check.date() == (2028, 2, 28)
            session.key('right'); assert check.date() == (2028, 2, 29)
            check.choose_day(15)
            late = check.add('Afternoon review', '16:45')
            allday = check.add('All day planning')
            early = check.add('Morning meeting', '09:10')
            assert [(a['id'], a['minute']) for a in check.items()] == [(allday, -1), (early, 550), (late, 1005)]
            # Pointer rows and ordinary Up/Down follow displayed sorted order.
            check.select_row(0); assert check.integer('selected') == allday
            session.key('down'); assert check.integer('selected') == early
            session.key('down'); assert check.integer('selected') == late
            pictures.append(str(session.screenshot('calendar-sorted-agenda-800x600.png')))
            print('PASS: actual grid date, leap-day month clamp, timed/all-day sorting and selection', flush=True)

            session.key('ret')
            assert check.draft()['edit_id'] == late
            original = check.items()
            check.field(2, 'Rescheduled review')
            check.field(0, '2028-02-30'); session.key('ret')
            assert check.draft()['date'] == '2028-02-30' and check.draft()['field'] == 0
            assert check.items() == original
            check.field(0, '2028-02-29'); check.field(1, '25:70'); session.key('ret')
            assert check.draft()['time'] == '25:70' and check.draft()['field'] == 1
            assert check.draft()['title'] == 'Rescheduled review' and check.items() == original
            pictures.append(str(session.screenshot('calendar-invalid-time-entry-kept.png')))
            check.field(1, '08:05'); session.key('ret')
            session.wait(lambda: not check.draft()['active'], 'Edited appointment committed')
            assert check.date() == (2028, 2, 29)
            assert next(a for a in check.items() if a['id'] == late) == dict(
                id=late, year=2028, month=2, day=29, minute=485, title='Rescheduled review')

            check.choose_day(15); check.select_row(1)
            assert check.integer('selected') == early
            before = check.items()
            session.key('delete'); assert check.integer('confirm') == 1 and check.items() == before
            session.key('esc'); assert not check.integer('confirm') and check.owner() and check.items() == before
            session.key('delete'); check.body(check.owner()['w'] - 50, 306)
            assert not check.integer('confirm') and check.items() == before
            session.key('delete'); session.key('ret')
            assert [a['id'] for a in check.items()] == [allday, late]

            session.key('n'); session.text('Cancel only after review')
            draft = check.draft(); check.cancel(); session.key('esc')
            assert check.draft() == draft
            check.cancel(); check.body(check.owner()['w'] - 50, 306)
            assert check.draft() == draft
            check.cancel(); session.key('ret')
            assert not check.draft()['active'] and [a['id'] for a in check.items()] == [allday, late]
            print('PASS: stable-ID editing, invalid date/time preservation, guarded delete and draft cancellation', flush=True)

            session.key('n'); session.text('Carry this unfinished entry')
            session.key('tab'); session.key('ctrl-a'); session.text('2028-0')
            session.key('tab'); session.text('09:')
            draft = check.draft()
            assert draft == dict(active=1, field=1, edit_id=0, date='2028-0', time='09:', title='Carry this unfinished entry')
            check.close('esc'); check.open(); assert check.draft() == draft
            session.key('ctrl-n'); assert check.draft() == draft
            check.close(); check.open(); assert check.draft() == draft
            check.save()
            expected = check.snapshot()
            pictures.append(str(session.screenshot('calendar-partial-draft-reopened.png')))
            check.close(); session.shutdown()
        expect_stopped(disk, expected, original_nodes)
        with session_for(disk, 'reboot') as session:
            session.boot(); check = CalendarCheck(session, build); check.open()
            assert check.snapshot() == expected
            pictures.append(str(session.screenshot('calendar-draft-and-appointments-after-reboot.png')))
            # Finish the recovered partial entry using the real All-day control.
            check.field(0, '2028-03-01')
            check.body(341, 359); assert check.draft()['time'] == ''
            session.key('ret'); assert not check.draft()['active']
            check.save(); recovered = check.snapshot()
            assert len(recovered['items']) == 3
            assert recovered['items'][-1]['title'] == draft['title'] and recovered['items'][-1]['minute'] == -1
            check.close(); session.shutdown()
        expect_stopped(disk, recovered, original_nodes)
        print('PASS: Escape/close/reopen, durable safe Shutdown, exact BCA1/CRC, reboot and finishing recovered draft', flush=True)
        checks += ['grid date and leap-day clamp', 'timed/all-day sorted selection',
                   'stable-ID edit and invalid date/time retain entry', 'guarded delete and Cancel/Keep',
                   'Escape/close/reopen retain partial entry', 'safe Shutdown and exact BCA1 CRC',
                   'real reboot restores appointments and partial draft', 'All-day control finishes recovered draft']

    if scenario in ('all', 'collision'):
        disk, original_nodes = make_disk(work, profile, collision=True)
        with session_for(disk, 'collision') as session:
            session.boot(); check = CalendarCheck(session, build); check.open()
            assert check.integer('ca_state') == CA_PATH_FAILED
            ident = check.add('Preserve path and retry', '11:25')
            session.key('ctrl-s')
            session.wait(lambda: check.integer('ca_state') == CA_PATH_FAILED, 'Occupied path is protected')
            assert check.integer('ca_pending') and check.items()[0]['id'] == ident
            pictures.append(str(session.screenshot('calendar-path-collision-preserved.png')))
            check.close(); check.open(); assert check.items()[0]['id'] == ident
            # Normal Files Cut/Paste moves the whole collision folder intact.
            # Nothing is deleted and no live disk image is read or changed.
            session.launch('files'); files = FilesCheck(session, build)
            files.root(); files.select(3); session.key('ret'); files.select(4); files.clipboard('ctrl-x')
            files.root(); files.select(1); session.key('ret'); files.clipboard('ctrl-v')
            session.wait(lambda: files.state()['selected_id'] == 4, 'Conflicting folder moved intact')
            session.key('ctrl-w'); check.open(); check.save(); expected = check.snapshot()
            pictures.append(str(session.screenshot('calendar-path-repaired-retry-saved.png')))
            check.close(); session.shutdown()
        expect_stopped(disk, expected, original_nodes)
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/Documents/calendar.v1/keep.txt')] == original_nodes[5]
        assert nodes[4]['parent'] == 1 and nodes[4]['directory']
        checks.append('occupied-path protection and non-destructive Files Cut/Paste repair with Ctrl+S retry')
        print('PASS: occupied Calendar path protected, collision bytes moved intact, retained agenda saved on retry', flush=True)

    if scenario in ('all', 'readonly'):
        disk = work / 'unknown-protected.img'
        disk.write_bytes(b'Ordinary unknown disk; do not format.'.ljust(data_layout(profile).sectors * 512, b'\0'))
        digest = hashlib.sha256(disk.read_bytes()).hexdigest()
        with session_for(disk, 'readonly') as session:
            session.boot(); check = CalendarCheck(session, build); check.open()
            check.add('Retained without a writable disk', '10:30')
            session.key('n'); session.text('Keep unsaved partial entry')
            session.key('ctrl-s')
            session.wait(lambda: check.integer('ca_state') == CA_SYNC_FAILED, 'Read-only storage failure reported')
            expected = check.snapshot()
            check.close(); check.open(); assert check.snapshot() == expected
            session.key('ctrl-s'); assert check.snapshot() == expected
            frame = check.o.integer('frame_count')
            session.shutdown_keys()
            session.wait(lambda: check.o.integer('open_menu') < 0 and check.o.integer('frame_count') > frame,
                         'Failed Shutdown returns to live desktop')
            assert session.process.poll() is None and check.owner() and check.snapshot() == expected
            pictures.append(str(session.screenshot('calendar-readonly-shutdown-keeps-work.png')))
        assert hashlib.sha256(disk.read_bytes()).hexdigest() == digest
        checks.append('read-only save/retry/Shutdown preserve accepted appointments, draft, and unknown disk digest')
        print('PASS: protected unknown disk unchanged; failed save/retry/Shutdown keep agenda and draft available', flush=True)

    result = dict(passed=True, profile=profile, scenario=scenario, build=str(build),
                  source=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  kernel_sha256=hashlib.sha256((build / 'kernel.bin').read_bytes()).hexdigest(),
                  work=str(work), sessions=sessions, screenshots=pictures, checks=checks)
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


def self_test():
    """Exercise the independent host reader on one small valid wire document."""
    data = bytearray(HEADER + 2 * RECORD + DRAFT)
    struct.pack_into('<4sHHHBBIIIII', data, 0, b'BCA1', 1, HEADER, 2, 1, 1, 3, 0, len(data), 0, 0)
    for index, (ident, minute, title) in enumerate(((2, 65535, b'All day'), (1, 550, b'Morning'))):
        offset = HEADER + index * RECORD
        struct.pack_into('<IHBBHBB', data, offset, ident, 2028, 2, 29, minute, len(title), 0)
        data[offset + 12:offset + 12 + len(title)] = title
    offset = HEADER + 2 * RECORD
    data[offset:offset + 6] = b'2028-0'
    data[offset + 11:offset + 14] = b'09:'
    data[offset + 17:offset + 21] = b'Kept'
    struct.pack_into('<I', data, 24, zlib.crc32(data))
    result = decode_agenda(bytes(data))
    assert [a['id'] for a in result['items']] == [2, 1]
    assert result['draft'] == dict(active=1, field=1, edit_id=0, date='2028-0', time='09:', title='Kept')
    assert result['next_id'] == 3
    print('PASS: independent BCA1 v1 host decoder and CRC self-test (no QEMU)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path, nargs='?')
    parser.add_argument('--profile', choices=('default', 'large'), default='default')
    parser.add_argument('--scenario', choices=('all', 'main', 'collision', 'readonly'), default='all')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.build:
        run(args.build.resolve(), args.profile, args.scenario)
    else:
        parser.error('BUILD is required unless --self-test is supplied')
