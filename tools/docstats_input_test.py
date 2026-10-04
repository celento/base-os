"""Use the seeded native DocStats app through normal Terminal keyboard input."""
import argparse
import json
import pathlib
import struct
import tempfile
import time

from elf_debug import DebugInfo
from init_data import initialize
from make_stats_fixture import document
from volume import load, resolve
from writer_input_test import WriterSession, WriterCheck


class TerminalCheck:
    def __init__(self, session, build):
        self.s = session
        self.ui = WriterCheck(session, build)
        self.size, self.fields = DebugInfo(pathlib.Path(build) / 'kernel.elf').structure('Terminal', 'src/term.c')
        assert self.size * 8 <= 0xc0000

    def front(self):
        return max((w for w in self.ui.o.windows() if w['open'] and not w['min']), key=lambda w: w['z'])

    def terminal(self, slot):
        raw = self.s.memory(self.s.layout['APPS_BASE'] + 0x300000 + self.size * slot, self.size)
        result = {name: struct.unpack_from('<i', raw, self.fields[name])[0]
                  for name in ('head', 'count', 'canvas_on', 'canvas_width', 'canvas_height')}
        assert 0 <= result['count'] <= 320 and 0 <= result['head'] < 320
        lines = raw[self.fields['lines']:self.fields['lines'] + 320 * 81]
        result['lines'] = [lines[((result['head'] + i) % 320) * 81:((result['head'] + i) % 320 + 1) * 81].split(b'\0')[0].decode('ascii')
                           for i in range(result['count'])]
        result['canvas'] = raw[self.fields['canvas']:self.fields['canvas'] + 320 * 200]
        return result

    def ready(self, slot):
        return 'Ready. R reloads, S saves a report, Q exits.' in self.terminal(slot)['lines']

    def assert_canvas(self, window, width, height):
        self.ui.move(self.ui.o.integer('fb_w') - 24, self.ui.o.integer('fb_h') - 65)
        target_h = 400 if width > 160 and window['h'] > 550 else 200 if window['h'] > 360 else 100
        available_h = window['h'] - 32 - 2 - 24 - 2 * 15 - 8
        target_h = min(target_h, available_h)
        target_w = width * target_h // height
        available_w = window['w'] - 2 - 24
        if target_w > available_w:
            target_w = available_w; target_h = height * target_w // width
        screen_w, screen_h = self.ui.o.integer('fb_w'), self.ui.o.integer('fb_h')
        ax, ay = window['x'] + 13, window['y'] + 45
        def presented():
            terminal = self.terminal(window['slot'])
            if (terminal['canvas_width'], terminal['canvas_height']) != (width, height):
                return False
            # The backbuffer may be midway through repaint. Observe the last
            # presented indexed pixels after the app has settled instead.
            pixels = self.s.memory(self.s.layout['PRESENT_BASE'], screen_w * screen_h)
            for y in range(target_h):
                row = terminal['canvas'][(y * height // target_h) * width:((y * height // target_h) + 1) * width]
                expected = bytes(row[x * width // target_w] for x in range(target_w))
                if pixels[(ay + y) * screen_w + ax:(ay + y) * screen_w + ax + target_w] != expected:
                    return False
            return True
        self.s.wait(presented, 'complete native canvas is presented pixel-for-pixel', 20)
        return target_w, target_h


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-docstats-input-data-'))
    disk = work / 'data.img'; initialize(disk)
    expected = document(); reports = {}; captures = []
    with WriterSession(build, 'docstats-input', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = TerminalCheck(session, build)
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/Documents/stats-sample.txt')]['data'] == expected
        session.launch('terminal'); session.text('start /Programs/docstats.bex'); session.key('ret')
        first = check.front(); assert first['kind'] == 11
        session.wait(lambda: check.ready(first['slot']), 'first DocStats stream completes')
        assert check.assert_canvas(check.front(), 320, 200) == (320, 200)
        captures.append(str(session.screenshot('docstats-native-canvas.png')))
        session.key('alt-ret')
        session.wait(lambda: check.front()['maximized'], 'Terminal maximized')
        assert check.assert_canvas(check.front(), 320, 200) == (640, 400)
        captures.append(str(session.screenshot('docstats-maximized.png')))
        for index in range(2):
            window = check.front(); slot = window['slot']; report = f'/Documents/stats-{slot + 1}.txt'
            session.key('s')
            def saved():
                try:
                    n = load(disk.read_bytes())[2]
                    reports[report] = n[resolve(n, report)]['data']
                    return True
                except ValueError:
                    return False
            session.wait(saved, 'native report synchronized')
            assert f'Bytes: {len(expected)}\n'.encode() in reports[report]
            assert f'Words: {len(expected.split())}\n'.encode() in reports[report]
            assert f'Lines: {len(expected.splitlines())}\n'.encode() in reports[report]
            assert f'Byte sum: {sum(expected)}\n'.encode() in reports[report]
            if index == 0:
                session.key('ctrl-n'); session.text('start /Programs/docstats.bex'); session.key('ret')
                second = check.front(); assert second['slot'] != first['slot']
                session.wait(lambda: check.ready(second['slot']), 'second independent native app')
        session.key('ctrl-w')
        session.key('q')
        session.wait(lambda: 'Native task finished.' in check.terminal(first['slot'])['lines'], 'DocStats returns to shell')
        session.text('start /Programs/counter.bex'); session.key('ret')
        session.wait(lambda: 'Counter: +/- changes by 10; Space pauses; S saves.' in check.terminal(first['slot'])['lines'],
                     'legacy Counter initialized')
        finished = check.terminal(first['slot'])['lines'].count('Native task finished.')
        session.key('q')
        session.wait(lambda: check.terminal(first['slot'])['lines'].count('Native task finished.') > finished,
                     'Counter stopped normally before static pixel comparison')
        check.assert_canvas(check.front(), 160, 100)
        captures.append(str(session.screenshot('docstats-legacy-canvas-restored.png')))
        session.wait(lambda: check.ui.o.integer('fs_touched') == 0, 'final disk synchronized')
    with WriterSession(build, 'docstats-reboot', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        session.boot()
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/Documents/stats-sample.txt')]['data'] == expected
        for path, data in reports.items():
            assert nodes[resolve(nodes, path)]['data'] == data
    result = {'passed': True, 'sample_bytes': len(expected), 'sample_words': len(expected.split()),
              'sample_lines': len(expected.splitlines()), 'report_paths': list(reports), 'screenshots': captures,
              'checks': ['compact sample seeding', 'real native streaming app', 'pixel-exact 320x200 and 640x400 display',
                         'two independent durable reports', 'legacy canvas reset', 'reboot persistence']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
