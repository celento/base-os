"""Normal production Files controls, screenshots and independently saved files.

Uses only QMP PS/2 keyboard/mouse input, screenshots and serial boot markers.
No debugger, guest-memory reads/writes or private kernel-state observations.
All disks are newly generated disposable fixtures.
"""
import argparse
import json
import pathlib
import tempfile
import time

from init_data import initialize
from qemu_session import DesktopSession
from volume import DATA_LAYOUT, encode_snapshot, load, resolve


class FilesSession(DesktopSession):
    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        time.sleep(.16)

    def origin(self):
        # Ordinary relative pointer movement to a known screen corner. No readback.
        for _ in range(20):
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': -80}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': -80}}]})
            time.sleep(.025)
        self.pointer = (0, 0)

    def move(self, x, y):
        px, py = self.pointer
        while (px, py) != (x, y):
            dx, dy = max(-80, min(80, x-px)), max(-80, min(80, y-py))
            events = [{'type': 'rel', 'data': {'axis': axis, 'value': delta}}
                      for axis, delta in [('x', dx), ('y', dy)] if delta]
            self.command('input-send-event', {'events': events})
            px, py = px+dx, py+dy
            time.sleep(.04)
        self.pointer = (x, y)

    def button(self, down):
        self.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
        time.sleep(.12)

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        self.button(False)

    def wheel(self, count):
        button = 'wheel-down' if count > 0 else 'wheel-up'
        for _ in range(abs(count)):
            self.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': True, 'button': button}},
                {'type': 'btn', 'data': {'down': False, 'button': button}}]})
            time.sleep(.08)

    def filter(self, text):
        self.key('ctrl-f'); self.text(text)

    def select_name(self, text):
        self.filter(text); self.key('down')

    def child_folder(self, name):
        self.select_name(name); self.key('ret')


def make_fixture(directory):
    disk = directory / 'files-data.img'
    initialize(disk)
    data = bytearray(disk.read_bytes())
    # Build an ordinary valid v4 snapshot before the first boot.
    nodes = {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Sort Demo', directory=1, app=0, data=b'', modified=844400000),
        2: dict(parent=0, name='Target', directory=1, app=0, data=b'', modified=844300000),
        3: dict(parent=1, name='Archive', directory=1, app=0, data=b'', modified=844200000),
        4: dict(parent=1, name='Alpha report.txt', directory=0, app=0, data=b'Alpha\n', modified=844200000),
        5: dict(parent=1, name='Beta report.txt', directory=0, app=0, data=b'Beta saved bytes.\n', modified=844400000),
        6: dict(parent=1, name='Zeta report.txt', directory=0, app=0, data=b'Zeta\n' * 40, modified=844300000),
        7: dict(parent=1, name='alpha data.bin', directory=0, app=0, data=bytes(range(64)), modified=0),
        8: dict(parent=3, name='nested.txt', directory=0, app=0, data=b'Nested content\n', modified=0),
    }
    for i in range(35):
        nodes[i+9] = dict(parent=1, name=f'item {i:02}.txt', directory=0, app=0,
                         data=f'Item {i}\n'.encode(), modified=844000000+i*86400)
    header, payload = encode_snapshot(nodes, DATA_LAYOUT, 1)
    start = DATA_LAYOUT.lbas[0] * 512
    data[start:start+512] = header.ljust(512, b'\0')
    data[start+512:start+512+len(payload)] = payload
    assert len(load(data)[2]) == len(nodes)
    disk.write_bytes(data)
    return disk


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-files-view-input-'))
    print(work, flush=True)
    disk = make_fixture(work)
    captures = []

    def nodes():
        return load(disk.read_bytes())[2]

    def exists(path):
        try:
            resolve(nodes(), path)
            return True
        except ValueError:
            return False

    with FilesSession(build, 'files-view', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot()
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.launch('files'); session.origin()
        session.child_folder('Sort Demo')
        captures.append(str(session.screenshot('files-name-800x600.png')))
        session.select_name('Beta report'); session.key('esc')
        session.key('ctrl-3'); session.key('ctrl-3')
        captures.append(str(session.screenshot('files-size-selection-retained.png')))
        session.key('ctrl-4')
        captures.append(str(session.screenshot('files-modified-selection-retained.png')))
        session.key('ctrl-c'); session.key('backspace'); session.child_folder('Target'); session.key('ctrl-v')
        session.wait(lambda: exists('/Target/Beta report.txt'), 'sorted selection copied to current folder')
        assert nodes()[resolve(nodes(), '/Target/Beta report.txt')]['data'] == b'Beta saved bytes.\n'
        session.key('backspace'); session.child_folder('Sort Demo')
        session.filter('REPORT')
        captures.append(str(session.screenshot('files-filter-case-insensitive.png')))
        # Clear through its visible button. Default Files: x=78,y=60,w=720 at 800x600.
        session.click(684, 168)
        session.filter('no matching name')
        captures.append(str(session.screenshot('files-filter-no-matches.png')))
        before = {n['name'] for n in nodes().values() if n['parent'] == 1}
        session.text('nnn'); session.key('ret'); session.key('ctrl-c')
        assert {n['name'] for n in nodes().values() if n['parent'] == 1} == before
        session.key('esc')
        session.select_name('Alpha report'); time.sleep(.5)
        session.click(138, 230)  # Label of the only matched file after the parent row.
        for _ in range(len('Alpha report.txt')):
            session.key('backspace')
        session.text('Renamed report.txt'); session.key('ret')
        session.wait(lambda: exists('/Sort Demo/Renamed report.txt'), 'filtered rename persisted')
        assert not exists('/Sort Demo/Alpha report.txt')
        captures.append(str(session.screenshot('files-renamed-result-revealed.png')))
        session.key('esc'); session.key('ctrl-x'); session.key('backspace')
        session.child_folder('Target'); session.key('ctrl-v')
        session.wait(lambda: exists('/Target/Renamed report.txt'), 'Cut moved exact renamed object')
        assert not exists('/Sort Demo/Renamed report.txt')
        assert nodes()[resolve(nodes(), '/Target/Renamed report.txt')]['data'] == b'Alpha\n'
        session.key('backspace'); session.child_folder('Sort Demo')
        session.filter('item'); session.key('down')
        session.move(400, 300); session.wheel(8)
        captures.append(str(session.screenshot('files-filter-scrolled.png')))
        session.key('ctrl-f'); session.key('ctrl-a'); session.text('item 01'); session.key('down')
        captures.append(str(session.screenshot('files-filter-scroll-reset.png')))
        # Two independently filtered views; closing the new window retains the old view.
        session.key('ctrl-n'); session.filter('Zeta');
        captures.append(str(session.screenshot('files-second-window-filter.png')))
        session.key('ctrl-w')
        captures.append(str(session.screenshot('files-first-window-filter-retained.png')))
        session.key('esc'); session.key('ctrl-2')
        session.move(796, 538); session.button(True); session.move(436, 258); session.button(False)
        captures.append(str(session.screenshot('files-minimum-360x200.png')))
        session.filter('missing')
        captures.append(str(session.screenshot('files-minimum-filter-no-matches.png')))
        # Visible Close on the compact bar; then a matching row scrolls into view.
        session.click(386, 168); session.filter('Beta report'); session.key('down')
        captures.append(str(session.screenshot('files-minimum-filter-selected.png')))
        session.key('esc'); session.key('alt-ret')
        session.key('ctrl-w')
        session.wait(lambda: exists('/Target/Renamed report.txt'), 'saved result remains available')
        time.sleep(6)  # Let normal session autosave record the closed Files window.

    with FilesSession(build, 'files-view-reboot', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        session.boot()
        assert nodes()[resolve(nodes(), '/Target/Beta report.txt')]['data'] == b'Beta saved bytes.\n'
        assert nodes()[resolve(nodes(), '/Target/Renamed report.txt')]['data'] == b'Alpha\n'
        assert not exists('/Sort Demo/Alpha report.txt') and not exists('/Sort Demo/Renamed report.txt')
        session.launch('files'); session.child_folder('Target')
        captures.append(str(session.screenshot('files-results-after-reboot.png')))

    result = {'passed_saved_file_checks': True,
              'checks': ['selection survives Size/Modified reordering', 'Paste destination is the current folder',
                         'filtered rename reveals exact result', 'identity-preserving Cut',
                         'empty-filter text does not create files', 'saved bytes after reboot'],
              'screenshots_for_visual_review': captures,
              'data_disk': str(disk)}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
