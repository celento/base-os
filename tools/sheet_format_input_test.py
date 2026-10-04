"""Production PS/2 Spreadsheet formats/widths, native/CSV and reboot recovery.

All edits go through actual keyboard/mouse controls. Bounded DWARF/QMP reads
observe metadata; native files are decoded independently after durable saves.
Uses only newly created disposable data disks, never a saved working volume.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

from sheet_input_test import SheetSession, SheetCheck, native, decode_document, contents
from writer_input_test import fixture


def run(build, profile='default'):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-sheet-formats-'))
    original = native([(0, 2, '0.125'), (1, 3, '=A1*2'), (2, 2, '-12.345'),
                       (3, 1, 'literal'), (26, 2, '123.456')])
    disk = fixture(work, [('formats.bsh', original)], profile)
    machine = ['-m', '128M' if profile == 'large' else '64M']
    captures = []
    with SheetSession(build, 'sheet-format-' + profile,
                      extra=machine + ['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = SheetCheck(session, build)
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.wait(lambda: check.o.integer('fb_w') == 800, '800-pixel display')
        session.launch('formats.bsh'); check.expect(0, '0.125', 2, 125)
        assert check.widths() == [104] * 26 and check.format(0) == 0
        session.key('ctrl-s')
        session.wait(lambda: check.o.integer('fs_touched') == 0, 'legacy save durable')
        assert contents(disk, '/Documents/formats.bsh') == original
        print('PASS: legacy v1 opens and resaves byte-identically with default metadata', flush=True)

        # Pointer and keyboard controls exercise all four formats, including a
        # formatted empty cell which the native file must retain explicitly.
        check.jump('A1'); check.toolbar(188, 50)
        session.wait(lambda: check.format(0) == 3, 'pointer Percent control')
        session.key('shift-right'); session.key('ctrl-3')
        session.wait(lambda: check.format(0) == 2 and check.format(1) == 2, 'currency over selected range')
        session.key('ctrl-z'); session.wait(lambda: check.format(0) == 3 and check.format(1) == 0, 'format undo')
        session.key('ctrl-y'); session.wait(lambda: check.format(0) == 2 and check.format(1) == 2, 'format redo')
        check.jump('A1'); session.key('ctrl-4')
        check.jump('C1'); session.key('ctrl-2')
        check.jump('D2'); session.key('ctrl-3')
        session.wait(lambda: check.format(0) == 3 and check.format(1) == 2 and
                     check.format(2) == 1 and check.format(29) == 2, 'all display formats applied')
        check.expect(0, '0.125', 2, 125); check.expect(1, '=A1*2', 3, 250)
        check.expect(2, '-12.345', 2, -12345)
        check.jump('A1'); session.key('shift-right'); session.key('ctrl-equal')
        session.wait(lambda: check.widths()[:2] == [120, 120], 'range widths increase')
        session.key('ctrl-z'); session.wait(lambda: check.widths()[:2] == [104, 104], 'width undo')
        session.key('ctrl-y'); session.wait(lambda: check.widths()[:2] == [120, 120], 'width redo')
        session.key('ctrl-0'); session.wait(lambda: check.widths()[:2] == [104, 104], 'width reset')
        check.toolbar(334, 50); session.wait(lambda: check.widths()[:2] == [120, 120], 'pointer width plus')
        check.jump('D1'); check.toolbar(266, 50)
        session.wait(lambda: check.widths()[3] == 88, 'pointer width minus')
        check.save_as('styled.bsh')
        styled = decode_document(contents(disk, '/Documents/styled.bsh'))
        assert styled['version'] == 2 and styled['widths'][:4] == [120, 120, 104, 88]
        assert styled['formats'][0] == 3 and styled['formats'][1] == 2 and styled['formats'][2] == 1
        assert styled['records'][29] == (0, b'') and styled['formats'][29] == 2
        assert styled['records'][0] == (2, b'0.125') and styled['records'][1] == (3, b'=A1*2')
        assert styled['records'][2] == (2, b'-12.345')
        session.key('ctrl-shift-e'); check.name('values.csv')
        session.wait(lambda: not check.o.integer('name_dlg') and not check.o.integer('fs_touched'), 'CSV export durable')
        assert contents(disk, '/Documents/values.csv') == b'0.125,0.25,-12.345,literal\r\n123.456,,,\r\n'
        check.jump('A1')
        captures.append(str(session.screenshot('sheet-formats-and-widths.png')))
        print('PASS: pointer/keyboard formats, range width controls, undo/redo, exact v2 and unformatted CSV', flush=True)

        session.key('ctrl-w'); session.launch('styled.bsh'); check.expect(1, '=A1*2', 3, 250)
        assert check.widths()[:4] == [120, 120, 104, 88] and check.format(29) == 2
        check.enter('D2', '2.5'); assert check.format(29) == 2
        check.jump('D2'); session.key('delete'); check.expect(29, '', 0)
        assert check.format(29) == 2
        session.key('ctrl-z'); check.expect(29, '2.5', 2, 2500)
        check.jump('A1'); session.key('ctrl-c'); check.jump('F1'); session.key('ctrl-v')
        check.expect(5, '0.125', 2, 125); assert check.format(5) == 3
        session.key('ctrl-z'); check.expect(5, '', 0); assert check.format(5) == 0
        print('PASS: reopening retains metadata, typed/cleared cells retain format, private copy transfers format', flush=True)

        # Exercise both width clamps and the partially visible final column in
        # a real minimum-width window. These inputs are ordinary key repeats.
        check.jump('Z128')
        for _ in range(16): session.key('ctrl-equal')
        session.wait(lambda: check.widths()[25] == 320, 'maximum width clamp')
        for _ in range(20): session.key('ctrl-minus')
        session.wait(lambda: check.widths()[25] == 48, 'minimum width clamp')
        for _ in range(20): session.key('ctrl-equal')
        session.wait(lambda: check.widths()[25] == 320, 'maximum width restored')
        win = check.owner(); check.move(win['x'] + win['w'] - 2, win['y'] + win['h'] - 2)
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
        session.wait(lambda: check.o.integer('resizing_win') >= 0, 'resize began')
        check.move(win['x'] + 360, win['y'] + 292)
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
        session.wait(lambda: check.owner()['w'] == 362 and check.owner()['h'] == 294, 'minimum size')
        check.jump('Z128'); session.text('wide last'); session.key('ret')
        check.expect(3327, 'wide last', 1)
        captures.append(str(session.screenshot('sheet-minimum-wide-last-column.png')))
        session.key('ctrl-home'); session.wait(lambda: check.state()['scroll_x'] == 0, 'Home reveals A')
        session.key('ctrl-end'); session.wait(lambda: check.selected()['caret'] == 3327 and
                     check.state()['scroll_x'] > 0, 'End reveals wide Z')
        print('PASS: 48/320-pixel clamps, minimum window, wide Z128, Home/End navigation', flush=True)

        # Dirty display metadata and a not-yet-committed cell editor must be
        # captured together without consuming the edit or changing source file.
        check.jump('C1'); session.key('ctrl-3'); session.key('ctrl-equal')
        check.jump('E3'); session.text('Pending format recovery')
        def recovery_saved():
            try:
                doc = decode_document(contents(disk, '/prefs/sheet-draft.bsh'))
                return (doc['formats'].get(2) == 2 and doc['widths'][2] == 120 and
                        doc['widths'][25] == 320 and doc['records'].get(56) == (1, b'Pending format recovery'))
            except (ValueError, AssertionError, KeyError):
                return False
        session.wait(recovery_saved, 'pending source plus metadata recovery durable', 90)
        assert check.state()['mode'] == 1 and check.cell(56)['kind'] == 0
        assert decode_document(contents(disk, '/Documents/styled.bsh')) == styled
        captures.append(str(session.screenshot('sheet-pending-format-recovery.png')))
    before_reboot = hashlib.sha256(disk.read_bytes()).hexdigest()
    with SheetSession(build, 'sheet-format-reboot-' + profile,
                      extra=machine + ['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
        check.expect(56, 'Pending format recovery', 1)
        assert check.format(0) == 3 and check.format(1) == 2 and check.format(2) == 2 and check.format(29) == 2
        assert check.widths()[:4] == [120, 120, 120, 88] and check.widths()[25] == 320
        check.expect(29, '2.5', 2, 2500); check.expect(3327, 'wide last', 1)
        session.key('ctrl-s')
        session.wait(lambda: decode_document(contents(disk, '/Documents/styled.bsh'))['records'].get(56) ==
                     (1, b'Pending format recovery') and not check.o.integer('fs_touched'), 'recovered metadata saved to bound source')
        saved = decode_document(contents(disk, '/Documents/styled.bsh'))
        assert saved['formats'][2] == 2 and saved['widths'][2] == 120 and saved['widths'][25] == 320
        check.jump('A1'); captures.append(str(session.screenshot('sheet-format-recovered.png')))
        print('PASS: real reboot restores formats/widths/pending text and saves matched bound source', flush=True)
    result = dict(passed=True, profile=profile, disk=str(disk), pre_reboot_disk_sha256=before_reboot,
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                  kernel_sha256=hashlib.sha256((build / 'kernel.bin').read_bytes()).hexdigest(),
                  build_identity=json.loads((build / 'build_info.json').read_text()),
                  screenshots=captures, checks=[
                      'legacy v1 byte-identical default resave', 'pointer and keyboard number formats',
                      'range widths and metadata undo/redo', 'v2 formatted-empty records and exact source',
                      'CSV retains underlying unformatted values', 'reopen/edit/delete/private clipboard metadata',
                      '48/320 pixel clamps and minimum-window Z128 navigation',
                      'pending source plus metadata recovery and matched bound save after real reboot'])
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--profile', choices=('default', 'large'), default='default')
    args = parser.parse_args(); run(args.build.resolve(), args.profile)
