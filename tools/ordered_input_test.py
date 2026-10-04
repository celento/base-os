"""Ordinary production PS/2 ordered-input gate, screenshots and offline pixels.

No guest-memory reads/writes, debugger, injected callbacks, deliberate faults or
live-volume reads. A fresh disposable volume is decoded only after shutdown.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from init_data import initialize
from paint_save_input_test import PaintSession
from files_view_input_test import FilesSession
from volume import load, resolve, DATA_LAYOUT, encode_snapshot

ROOT = Path(__file__).resolve().parents[1]

class OrderedSession(PaintSession):
    origin = FilesSession.origin
    move = FilesSession.move

    def button(self, down, delay=.04):
        self.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(delay)

    def click(self, x, y):
        self.move(x, y); self.button(True); self.button(False)

    def release_motion(self, dx, dy):
        events = [{'type': 'rel', 'data': {'axis': axis, 'value': delta}}
                  for axis, delta in (('x', dx), ('y', dy)) if delta]
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': False}})
        self.command('input-send-event', {'events': events})
        self.pointer = self.pointer[0] + dx, self.pointer[1] + dy
        time.sleep(.15)


def run(build, output):
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'ordered-data.img'; initialize(disk)
    fixture = {0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
               1: dict(parent=0, name='Picker fixture', directory=1, app=0, data=b'', modified=0),
               2: dict(parent=1, name='selection.txt', directory=0, app=0,
                       data=b'File picker double-click opened this document.\n', modified=1)}
    header, payload = encode_snapshot(fixture, DATA_LAYOUT, 1)
    raw = bytearray(disk.read_bytes()); offset = DATA_LAYOUT.lbas[0] * 512
    raw[offset:offset+512] = header.ljust(512, b'\0')
    raw[offset+512:offset+512+len(payload)] = payload; disk.write_bytes(raw)
    report = dict(revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  hashes={name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                          for name in ('kernel.bin', 'kernel.elf', 'boot.bin')}, checks=[], screenshots=[])
    with OrderedSession(build, 'ordered-input', extra=(
            '-drive', f'file={disk},format=raw,index=0,if=ide')) as session:
        print('Ordered input evidence:', output, session.directory, flush=True)
        session.boot(); session.launch('paint'); session.origin()
        report['screenshots'].append(session.shot(output, 'paint', 'initial'))
        # Fresh default Paint is (80,48,760,500), with a 3x canvas at (220,156).
        session.click(400, 300)  # one pencil dot, logical (60,48)
        session.click(170, 98)  # Line tool
        session.move(280, 216); session.button(True)
        session.release_motion(60, 0)  # logical (20,20) -> (40,20), motion carried by UP
        report['checks'].append('rapid DOWN/UP and final-UP line coordinates')

        session.click(202, 98)  # Rectangle tool
        session.move(280, 336); session.button(True); session.move(370, 336)
        session.key('ctrl-spc'); session.move(950, 580); session.button(False); session.key('esc')
        report['checks'].append('launcher cancels held Paint shape without committing')

        session.move(520, 396); session.button(True); session.move(550, 411)
        session.release_motion(0, 100)
        session.move(580, 426)  # no stale rectangle follows the released cursor
        report['checks'].append('rectangle release outside canvas clears gesture')
        report['screenshots'].append(session.shot(output, 'paint', 'drawn-and-cancelled'))

        # Ordinary title drag ending in a movement+UP packet; then Save through
        # the relocated window's keyboard controls, proving desktop progress.
        session.move(500, 60); session.button(True); session.release_motion(40, 30)
        report['screenshots'].append(session.shot(output, 'paint', 'moved-window'))
        session.key('ctrl-s'); session.name('ordered.pbm')
        report['screenshots'].append(session.shot(output, 'paint', 'saved'))
        session.key('ctrl-o'); time.sleep(.25)
        session.click(500, 244); session.click(500, 244); time.sleep(.5)
        report['screenshots'].append(session.shot(output, 'picker', 'folder-open'))
        session.click(500, 244); session.click(500, 244); time.sleep(.4)
        report['screenshots'].append(session.shot(output, 'picker', 'document-open'))
        session.key('ctrl-w')
        session.key('ctrl-o'); session.key('esc')
        report['checks'].append('file picker folder/file double-click and Escape dismissal')
        session.key('ctrl-w'); session.launch('terminal')
        session.text('echo ordered-ingress'); session.key('ret')
        session.text('basic /Programs/demo.bas'); session.key('ret'); session.key('z'); time.sleep(.7)
        session.text('echo synchronous-return'); session.key('ret')
        report['screenshots'].append(session.shot(output, 'terminal', 'basic-and-return'))
        report['checks'].append('Terminal typing and synchronous BASIC INKEY return')
        session.move(300, 400)
        for button in ('wheel-up', 'wheel-up', 'wheel-down'):
            session.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': True}},
                {'type': 'btn', 'data': {'button': button, 'down': False}}]})
            time.sleep(.15)
        report['screenshots'].append(session.shot(output, 'terminal', 'wheel-scrollback'))
        for key in ('f10', 'right', 'right', 'down', 'down', 'ret'): session.key(key)
        report['screenshots'].append(session.shot(output, 'saver', 'active'))
        session.key('spc')
        report['screenshots'].append(session.shot(output, 'saver', 'wake-consumed'))
        report['checks'].append('wheel scrollback, menu saver activation and consumed wake')
        report['screenshots'].append(session.shutdown(output, 'final'))
        (output / 'serial.log').write_text(session.log.read_text())
        assert 'PANIC:' not in session.log.read_text()
    nodes = load(disk.read_bytes())[2]
    actual = nodes[resolve(nodes, '/Pictures/ordered.pbm')]['data']
    pixels = bytearray([15]) * 16000
    for y in range(47, 50):
        for x in range(59, 62): pixels[y * 160 + x] = 0
    for x in range(20, 41): pixels[20 * 160 + x] = 0
    for x in range(100, 111): pixels[80 * 160 + x] = pixels[85 * 160 + x] = 0
    for y in range(80, 86): pixels[y * 160 + 100] = pixels[y * 160 + 110] = 0
    expected = struct.pack('<4sHH', b'BOS1', 160, 100) + pixels
    (output / 'actual.pbm').write_bytes(actual)
    assert actual == expected, 'Saved pixels differ from dot + final-UP line + released rectangle; inspect screenshots/actual.pbm'
    report['checks'].append('exact offline BOS1 pixels, including absence of cancelled shape')
    report['saved_sha256'] = hashlib.sha256(actual).hexdigest()
    report['passed'] = True
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path); parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix='baseos-ordered-input-')) / 'evidence'
    run(args.build.resolve(), output.resolve())
