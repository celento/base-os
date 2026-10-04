"""Exercise downloads and Browser Save through production-kernel PS/2 routing.

Uses build/boot.bin and build/kernel.bin unchanged, a loopback HTTP fixture,
QMP keyboard input/read-only observations, and disposable floppy/data images.
Never reads or modifies build/baseos.img or build/baseos-data.img.
Usage: python3 tools/download_input_test.py build
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import struct
import tempfile
import threading
import time
from init_data import initialize
from qemu_session import DesktopSession
from volume import load, resolve

FILE_BYTES = 2097152
BINARY = bytes((i * 37 + (i >> 16) + 91) & 255 for i in range(FILE_BYTES))
PAGE = (b'<!doctype html><title>Keyboard saved page</title><h1>Saved with Ctrl+S</h1>'
        b'<p>Original source &amp; exact bytes.</p>'
        b'<script>this source is saved but never executed</script>\n')
DOWNLOAD_ACTIVE, DOWNLOAD_DONE, DOWNLOAD_CANCELLED = 1, 2, 4


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    paths = []
    cancel_release = threading.Event()

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.paths.append(self.path)
        if self.path not in ('/cancel', '/binary', '/page'):
            self.send_error(404)
            return
        data = PAGE if self.path == '/page' else BINARY
        try:
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Content-Type', 'text/html' if self.path == '/page' else 'application/octet-stream')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            if self.path == '/cancel':
                self.wfile.write(data[:32768])
                self.wfile.flush()
                # Ordinary bounded slow-server response. The test cancels using
                # actual Terminal input before allowing the remainder to send.
                self.cancel_release.wait(10)
                self.wfile.write(data[32768:])
            elif self.path == '/binary':
                for offset in range(0, len(data), 8192):
                    self.wfile.write(data[offset:offset+8192])
                    self.wfile.flush()
                    time.sleep(.015)
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def terminal_memory(session):
    return session.memory(session.layout['APPS_BASE'] + 0x300000, 0xC0000)


def browser_memory(session):
    return session.memory(session.layout['BROWSER_BASE'], session.layout['BROWSER_CAPACITY'])


def download_status(session):
    # Read the public DownloadStatus prefix from its fixed arena. These are
    # observations only; all starts, cancellation and saves use PS/2 input.
    data = session.memory(session.layout['DOWNLOAD_BASE'], 1980)
    state, http_state, http_status, file_id, request_id, received, limit = struct.unpack_from('<4i3I', data)
    message = data[1820:1980].split(b'\0', 1)[0].decode('ascii', 'replace')
    return dict(state=state, http_state=http_state, http_status=http_status,
                file_id=file_id, request_id=request_id, received=received,
                limit=limit, message=message)


def command(session, text):
    assert len(text) <= 80, 'Fixture command exceeds Terminal input capacity'
    session.text(text)
    session.key('ret')


def wait_terminal(session, text):
    session.wait(lambda: text.encode() + b'\0' in terminal_memory(session),
                 'Terminal output missing: ' + text)


def navigate(session, url):
    session.key('ctrl-l')
    session.text(url)
    session.key('ret')


def verify_volume(data):
    _, generation, nodes = load(data.read_bytes())
    assert nodes[resolve(nodes, '/network.bin')]['data'] == BINARY, 'Downloaded binary bytes differ'
    for path in ('/Downloads/page.html', '/Downloads/page-2.html'):
        assert nodes[resolve(nodes, path)]['data'] == PAGE, 'Saved original page differs: ' + path
    try:
        resolve(nodes, '/cancelled.bin')
    except ValueError:
        pass
    else:
        raise AssertionError('Cancelled transfer created a destination')
    return generation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build.resolve()
    evidence = pathlib.Path(tempfile.mkdtemp(prefix='baseos-download-input-evidence-'))
    data = evidence / 'data.img'
    initialize(data)
    print(f'Production-kernel download input evidence: {evidence}', flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f'http://10.0.2.2:{server.server_port}'
    extra = ('-drive', f'file={data},format=raw,index=0,if=ide,cache=writeback',
             '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0')
    evidence_info = dict(kernel_sha256=hashlib.sha256((build / 'kernel.bin').read_bytes()).hexdigest())
    try:
        with DesktopSession(build, 'download-input', extra=extra) as session:
            evidence_info['first_boot'] = str(session.directory)
            print('First production boot:', session.directory, flush=True)
            try:
                session.boot()
                session.launch('terminal')
                command(session, f'download {origin}/cancel /cancelled.bin')
                session.wait(lambda: download_status(session)['state'] == DOWNLOAD_ACTIVE
                             and download_status(session)['received'] >= 32768,
                             'Production loop did not advance background download', seconds=8)
                command(session, 'downloads')
                wait_terminal(session, 'Download in progress')
                wait_terminal(session, 'Received bytes: 32768')
                evidence_info['progress'] = download_status(session)
                session.screenshot('download-progress.png')
                command(session, 'cancel')
                session.wait(lambda: download_status(session)['state'] == DOWNLOAD_CANCELLED,
                             'Terminal cancellation did not route to the download service')
                wait_terminal(session, 'Download cancelled. No file was saved.')
                Fixture.cancel_release.set()
                session.screenshot('download-cancelled.png')
                print('PS/2 Terminal start, visible progress and cancellation passed.', flush=True)

                command(session, f'download {origin}/binary /network.bin')
                session.wait(lambda: download_status(session)['state'] == DOWNLOAD_ACTIVE
                             and download_status(session)['received'] >= 32768,
                             'Binary download did not start receiving', seconds=8)
                # Closing the initiating window must leave the background job
                # alive. A reopened Terminal gets no stale completion output.
                session.key('ctrl-w')
                session.launch('terminal')
                command(session, 'echo New terminal remains usable')
                wait_terminal(session, 'New terminal remains usable')
                assert b'Download complete\0' not in terminal_memory(session), 'Stale completion appeared in a reused Terminal'
                session.wait(lambda: download_status(session)['state'] == DOWNLOAD_DONE,
                             'Production loop did not commit the complete binary', seconds=18)
                status = download_status(session)
                assert status['received'] == FILE_BYTES and status['http_status'] == 200, status
                command(session, 'downloads')
                wait_terminal(session, 'Download complete')
                wait_terminal(session, 'Received bytes: 2097152')
                session.screenshot('download-complete.png')
                # A second command with the same path must fail before issuing
                # another HTTP request and leave the complete bytes intact.
                command(session, f'download {origin}/binary /network.bin')
                wait_terminal(session, 'Destination already exists. Choose another name; nothing was replaced.')
                assert Fixture.paths.count('/binary') == 1, Fixture.paths
                print('PS/2 close/reopen, complete 2 MiB download and non-overwrite passed.', flush=True)

                session.launch('browser')
                navigate(session, origin + '/page')
                session.wait(lambda: PAGE + b'\0' in browser_memory(session)
                             and b'HTTP 200 | ' in browser_memory(session), 'Browser HTTP page did not finish')
                session.key('ctrl-s')
                session.wait(lambda: b'Saved original page: /Downloads/page.html' in browser_memory(session),
                             'Browser Ctrl+S was not routed to Save')
                session.key('ctrl-s')
                session.wait(lambda: b'Saved original page: /Downloads/page-2.html' in browser_memory(session),
                             'Repeated Browser Ctrl+S did not choose a unique destination')
                navigate(session, 'file:///Downloads/page.html')
                session.wait(lambda: b'Local file | HTTP only browser\0' in browser_memory(session)
                             and PAGE + b'\0' in browser_memory(session), 'Saved page did not open locally')
                session.screenshot('browser-offline-page.png')
                # Autosave completion is observed through the production service,
                # rather than invoking a private function or flushing guest RAM.
                session.wait(lambda: 'disk is synchronized' in download_status(session)['message'],
                             'Production disk autosave did not finish', seconds=15)
                print('Real Browser Ctrl+S, unique saves and offline page reopening passed.', flush=True)
            except Exception:
                if session.process.poll() is None:
                    session.screenshot('failure.png')
                raise
        evidence_info['first_generation'] = verify_volume(data)
        requests_before_reboot = len(Fixture.paths)
        with DesktopSession(build, 'download-input-reboot', extra=extra) as session:
            evidence_info['reboot'] = str(session.directory)
            print('Second production boot:', session.directory, flush=True)
            try:
                session.boot()
                session.launch('terminal')
                command(session, 'stat /network.bin')
                wait_terminal(session, 'Bytes: 2097152')
                session.launch('browser')
                navigate(session, 'file:///Downloads/page.html')
                session.wait(lambda: b'Local file | HTTP only browser\0' in browser_memory(session)
                             and PAGE + b'\0' in browser_memory(session), 'Saved page did not reopen after reboot')
                session.screenshot('browser-offline-after-reboot.png')
                assert len(Fixture.paths) == requests_before_reboot, 'Offline reopening issued an HTTP request'
            except Exception:
                if session.process.poll() is None:
                    session.screenshot('failure.png')
                raise
        evidence_info['reboot_generation'] = verify_volume(data)
        evidence_info['binary_sha256'] = hashlib.sha256(BINARY).hexdigest()
        evidence_info['page_sha256'] = hashlib.sha256(PAGE).hexdigest()
        evidence_info['requests'] = Fixture.paths
        (evidence / 'verification.json').write_text(json.dumps(evidence_info, indent=2) + '\n')
        print('Production PS/2 routing passed: progress/cancel, 2 MiB completion, Terminal close/reopen, '
              'non-overwrite, Browser Ctrl+S, exact file bytes, and offline page after a second boot.', flush=True)
        print('Binary SHA-256:', evidence_info['binary_sha256'], flush=True)
    finally:
        Fixture.cancel_release.set()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
