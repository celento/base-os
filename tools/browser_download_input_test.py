"""Browser download controls through production PS/2 input and real HTTP.
Only disposable disks and loopback fixtures are used. Memory reads observe
state; all app actions use physical keyboard/mouse input, never guest calls.
Usage: python3 tools/browser_download_input_test.py build
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import struct
import subprocess
import tempfile
import threading
import time
from download_input_test import browser_memory, download_status, command
from init_data import initialize
from qemu_session import DesktopSession
from volume import load, resolve

BINARY = bytes((i * 37 + 91) & 255 for i in range(90001))
PAGE = (b'<title>Browser download controls</title><a href="/sound.wav?picked=1#track">Get sound</a>'
        b'<p>Original page stays readable during downloads.</p><script>preserved original source</script>')
ACTIVE, DONE, CANCELLED = 1, 2, 4


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    paths = []
    release = {name: threading.Event() for name in ('/cancel.bin', '/held.wav', '/terminal.bin')}

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.paths.append(self.path)
        path = self.path.split('?', 1)[0]
        payload = PAGE if path == '/index' else BINARY
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'text/html' if path == '/index' else 'application/octet-stream')
            self.send_header('Content-Length', str(len(payload)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            if path in self.release:
                self.wfile.write(payload[:32768]); self.wfile.flush()
                self.release[path].wait(12)
                self.wfile.write(payload[32768:])
            else:
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def type_url(session, url):
    session.key('ctrl-l')
    special = {'?': 'shift-slash', '=': 'equal', '#': 'shift-3', '&': 'shift-7'}
    for char in url:
        if char in special:
            session.key(special[char])
        else:
            session.text(char)


def present(session, text):
    return text.encode() in browser_memory(session)


def browser_window(session, symbols):
    data = session.memory(symbols['wins'], 8 * 56)
    windows = [struct.unpack_from('<14i', data, i * 56) for i in range(8)]
    w = next(w for w in windows if w[0] == 20 and w[6])
    return w[1] + 1, w[2] + 33, w[3] - 2, w[4] - 34


def move(session, symbols, x, y):
    for _ in range(35):
        mx = struct.unpack('<i', session.memory(symbols['mouse_x'], 4))[0]
        my = struct.unpack('<i', session.memory(symbols['mouse_y'], 4))[0]
        if (mx, my) == (x, y):
            return
        dx, dy = max(-90, min(90, x - mx)), max(-90, min(90, y - my))
        events = [{'type': 'rel', 'data': {'axis': axis, 'value': value}}
                  for axis, value in (('x', dx), ('y', dy)) if value]
        session.command('input-send-event', {'events': events}); time.sleep(.04)
    raise AssertionError(f'PS/2 pointer did not reach {x}, {y}: {mx}, {my}')


def button(session, down):
    session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
    time.sleep(.08)


def click(session, symbols, x, y):
    move(session, symbols, x, y); button(session, True); button(session, False)


def control(session, symbols, index):
    x, y, _, _ = browser_window(session, symbols)
    left = 8 + sum(width + 4 for width in (88, 58, 78, 80)[:index])
    click(session, symbols, x + left + 12, y + 85)


def finished(session, path):
    session.wait(lambda: download_status(session)['state'] == DONE, 'Download did not complete')
    session.wait(lambda: present(session, path) and present(session, 'Complete: 90001 bytes'), 'Browser completion missing')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    build = parser.parse_args().build.resolve()
    symbols = {p[2]: int(p[0], 16) for line in subprocess.check_output(['nm', '-n', str(build / 'kernel.elf')], text=True).splitlines() if len(p := line.split()) == 3}
    evidence = pathlib.Path(tempfile.mkdtemp(prefix='baseos-browser-download-evidence-'))
    data = evidence / 'data.img'; initialize(data)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f'http://10.0.2.2:{server.server_port}'
    extra = ('-drive', f'file={data},format=raw,index=0,if=ide,cache=writeback', '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0')
    info = {'kernel_sha256': hashlib.sha256((build / 'kernel.bin').read_bytes()).hexdigest()}
    print('Browser download evidence:', evidence, flush=True)
    try:
        with DesktopSession(build, 'browser-download-input', extra=extra) as session:
            info['session'] = str(session.directory)
            print('Production session:', session.directory, flush=True)
            try:
                session.boot(); session.launch('browser')
                type_url(session, origin + '/cancel.bin?name=other.wav#fragment'); session.key('ctrl-d')
                session.wait(lambda: download_status(session)['state'] == ACTIVE and download_status(session)['received'] >= 32768, 'Direct Ctrl+D did not download')
                session.wait(lambda: present(session, 'Downloading: 32768 bytes'), 'Visible byte progress missing')
                session.screenshot('progress.png'); control(session, symbols, 1)
                session.wait(lambda: download_status(session)['state'] == CANCELLED, 'Mouse Cancel failed')
                Fixture.release['/cancel.bin'].set(); session.screenshot('cancelled.png')
                type_url(session, origin + '/sound.wav?download=1#track'); session.key('ctrl-d')
                finished(session, '/Downloads/sound.wav'); session.key('ctrl-d'); finished(session, '/Downloads/sound-2.wav')
                assert Fixture.paths.count('/sound.wav?download=1') == 2, Fixture.paths
                type_url(session, origin + '/index'); session.key('ret')
                session.wait(lambda: PAGE + b'\0' in browser_memory(session) and present(session, 'HTTP 200 | '), 'Page did not load')
                control(session, symbols, 2)
                x, y, _, _ = browser_window(session, symbols)
                click(session, symbols, x + 20, y + 126)
                session.wait(lambda: present(session, 'Selected link | Download'), 'Mouse link selection failed')
                before = len(Fixture.paths); control(session, symbols, 0); finished(session, '/Downloads/sound-3.wav')
                assert Fixture.paths[before:] == ['/sound.wav?picked=1'], Fixture.paths
                session.key('ctrl-s'); session.wait(lambda: present(session, 'Saved original page: /Downloads/page.html'), 'Original HTML Ctrl+S changed')
                # Resize through the actual bottom-right window edge to minimum.
                x, y, w, h = browser_window(session, symbols)
                move(session, symbols, x + w, y + h); button(session, True)
                move(session, symbols, x + 360, y + 200); button(session, False)
                assert browser_window(session, symbols)[2:] == (360, 200), browser_window(session, symbols)
                session.screenshot('minimum-width.png'); session.key('end'); session.screenshot('minimum-width-details.png')
                session.key('alt-ret')
                type_url(session, origin + '/held.wav'); session.key('ctrl-d')
                session.wait(lambda: download_status(session)['state'] == ACTIVE and download_status(session)['received'] >= 32768, 'Held download did not begin')
                session.key('ctrl-w'); session.launch('browser'); assert download_status(session)['state'] == ACTIVE
                Fixture.release['/held.wav'].set(); finished(session, '/Downloads/held.wav')
                session.screenshot('complete-after-reopen.png')
                session.launch('terminal'); command(session, f'download {origin}/terminal.bin /terminal.bin')
                session.wait(lambda: download_status(session)['state'] == ACTIVE and download_status(session)['received'] >= 32768, 'Terminal fixture did not start')
                request_id = download_status(session)['request_id']; session.launch('browser')
                type_url(session, origin + '/not-started.bin'); session.key('ctrl-d')
                session.wait(lambda: present(session, 'running in another app'), 'Busy owner warning missing')
                control(session, symbols, 1); session.key('esc'); session.key('ctrl-w')
                assert download_status(session)['state'] == ACTIVE and download_status(session)['request_id'] == request_id
                Fixture.release['/terminal.bin'].set()
                session.wait(lambda: download_status(session)['state'] == DONE, 'Terminal download was interrupted')
                session.wait(lambda: 'disk is synchronized' in download_status(session)['message'], 'Disk did not autosync', seconds=15)
            except Exception:
                if session.process.poll() is None: session.screenshot('failure.png')
                raise
        _, generation, nodes = load(data.read_bytes())
        for path in ('/Downloads/sound.wav', '/Downloads/sound-2.wav', '/Downloads/sound-3.wav', '/Downloads/held.wav', '/terminal.bin'):
            assert nodes[resolve(nodes, path)]['data'] == BINARY, path
        assert nodes[resolve(nodes, '/Downloads/page.html')]['data'] == PAGE
        for path in ('/Downloads/cancel.bin', '/Downloads/not-started.bin'):
            try: resolve(nodes, path)
            except ValueError: pass
            else: raise AssertionError('Unexpected file: ' + path)
        info.update(generation=generation, requests=Fixture.paths, binary_sha256=hashlib.sha256(BINARY).hexdigest())
        (evidence / 'verification.json').write_text(json.dumps(info, indent=2) + '\n')
        print('PASS: direct/mouse-link downloads, byte progress, cancel, collisions, minimum window, close/reopen, other-app ownership, exact binary and HTML files.', flush=True)
    finally:
        for event in Fixture.release.values(): event.set()
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
