"""Normal Paint Save dialog gate using PS/2, screenshots and offline disk checks.

Uses a fresh disposable IDE volume, the unmodified production kernel, and normal
System -> Shutdown between phases. No guest-memory reads, debugger, injected
kernel calls, live-volume reads, fault probes or synthetic full-storage UI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import struct

from init_data import initialize
from qemu_session import DesktopSession
from volume import DATA_LAYOUT, encode_snapshot, load, resolve

ROOT = Path(__file__).resolve().parents[1]


class PaintSession(DesktopSession):
    def memory(self, *args, **kwargs):
        raise AssertionError('Paint verification forbids guest-memory observation')

    def command(self, name, arguments=None):
        assert name in ('qmp_capabilities', 'send-key', 'input-send-event', 'screendump')
        return super().command(name, arguments)

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        time.sleep(.18)

    def shot(self, directory, phase, name):
        time.sleep(.6)
        source = self.screenshot(name + '.png')
        target = directory / (phase + '-' + name + '.png')
        target.write_bytes(source.read_bytes())
        return target.name

    def shutdown(self, directory, phase):
        for key in ('f10', 'right', 'right', 'down', 'down', 'down'):
            self.key(key)
        shot = self.shot(directory, phase, 'shutdown-menu')
        self.key('ret')
        assert self.process.wait(timeout=40) == 0
        return shot

    def name(self, name):
        # The Save field is append/backspace, not a select-all text editor.
        for _ in range(23):
            self.key('backspace')
        self.text(name)
        self.key('ret')
        time.sleep(.7)

    def dot(self):
        # The fresh default desktop is 1280x720. The preceding screenshot shows
        # Paint's canvas at (220,156), rendered at 3x. Clamp to the screen origin
        # with normal relative PS/2 motion, then click logical canvas (60,48).
        for _ in range(14):
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': -100}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': -100}}]})
            time.sleep(.04)
        for _ in range(4):
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': 100}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': 75}}]})
            time.sleep(.04)
        for down in (True, False):
            self.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
            time.sleep(.2)


def run(build, work):
    work.mkdir(parents=True, exist_ok=False)
    disk = work / 'data.img'
    initialize(disk)
    nodes = {
        0: dict(name='', parent=-1, directory=1, app=0, data=b'', modified=0),
        1: dict(name='guard.txt', parent=0, directory=0, app=0,
                data=b'Preserve this ordinary file.\n', modified=456)}
    guard = nodes[1].copy()
    header, payload = encode_snapshot(nodes, DATA_LAYOUT, 1)
    data = bytearray(disk.read_bytes())
    start = DATA_LAYOUT.lbas[0] * 512
    data[start:start + 512] = header.ljust(512, b'\0')
    data[start + 512:start + 512 + len(payload)] = payload
    disk.write_bytes(data)
    report = dict(source=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  artifacts={name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                             for name in ('kernel.bin', 'kernel.elf', 'boot.bin')}, phases=[])
    for phase in ('cancel', 'save', 'reboot'):
        shots = []
        with PaintSession(build, 'paint-' + phase, extra=(
                '-drive', f'file={disk},format=raw,index=0,if=ide')) as session:
            print(phase, session.directory, flush=True)
            session.boot()
            session.launch('paint')
            shots.append(session.shot(work, phase, 'canvas'))
            if phase == 'cancel':
                session.key('ctrl-s')
                shots.append(session.shot(work, phase, 'save-open'))
                session.key('esc')
                shots.append(session.shot(work, phase, 'save-cancel'))
            elif phase == 'save':
                session.key('ctrl-s')
                session.name('first.pbm')
                shots.append(session.shot(work, phase, 'first-saved'))
                session.dot()
                shots.append(session.shot(work, phase, 'changed-canvas'))
                session.key('ctrl-s')
                session.name('first.pbm')
                shots.append(session.shot(work, phase, 'duplicate-rejected'))
                session.name('second.pbm')
                shots.append(session.shot(work, phase, 'unique-retry-saved'))
            else:
                shots.append(session.shot(work, phase, 'restored-canvas'))
            shots.append(session.shutdown(work, phase))
        # QEMU has exited and released the image before any snapshot is decoded.
        serial = session.log.read_text()
        (work / (phase + '-serial.log')).write_text(serial)
        assert 'DESKTOP-READY\n' in serial and 'PANIC:' not in serial
        data = disk.read_bytes()
        slot, generation, nodes = load(data)
        assert nodes[resolve(nodes, '/guard.txt')] == guard
        files = {}
        if phase == 'cancel':
            assert not any(n['parent'] == 0 and n['name'] == 'Pictures' for n in nodes.values())
        else:
            for name in ('first.pbm', 'second.pbm'):
                actual = nodes[resolve(nodes, '/Pictures/' + name)]['data']
                pixels = bytearray([15]) * 16000
                if name == 'second.pbm':
                    for y in range(47, 50):
                        for x in range(59, 62):
                            pixels[y * 160 + x] = 0
                assert actual == struct.pack('<4sHH', b'BOS1', 160, 100) + pixels
                files[name] = dict(bytes=len(actual), sha256=hashlib.sha256(actual).hexdigest())
        report['phases'].append(dict(phase=phase, passed=True, slot=slot, generation=generation,
                                     disk_sha256=hashlib.sha256(data).hexdigest(), screenshots=shots,
                                     files=files, session=str(session.directory)))
        (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    report['passed'] = True
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Paint normal-input save checks passed:', work, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix='baseos-paint-save-')) / 'evidence'
    run(args.build.resolve(), output.resolve())
