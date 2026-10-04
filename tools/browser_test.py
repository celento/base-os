"""Real graphical browser + HTTP integration in a disposable headless QEMU.
Uses a loopback HTTP fixture and never accesses the normal saved disk image.
Usage: python3 tools/browser_test.py build
"""
import argparse
import http.server
import json
import os
import select
import pathlib
import shutil
import subprocess
import tempfile
import threading
import time
from layout import constants
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]
INDEX = b'''<!doctype html><html><head><title>BaseOS live HTTP</title></head><body>
<a href="/next">Open the next page</a>
<h1>A real web page, in BaseOS</h1>
<p>This page arrived through QEMU's RTL8139 network adapter, IPv4, TCP and HTTP.</p>
<h2>Try the links</h2>
<ul><li><a href="/next">Next page</a></li><li><a href="/redirect">An HTTP redirect</a></li>
<li><a href="/secure">An HTTPS redirect, with an honest limitation</a></li></ul>
<h2>Small and readable</h2>
<p>Headings, lists, <strong>bold text</strong>, entities &amp; clickable links work.</p>
<p>No CSS, JavaScript, images, forms or TLS. HTTP is unencrypted.</p>
<h2>Keyboard controls</h2><p>Ctrl+L: address. Alt+Left/Right: history. F5: reload. Escape: stop.</p>
<pre>BaseOS / Browser\nC + x86 / No Linux layer</pre></body></html>'''


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    paths = []

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.paths.append(self.path)
        try:
            if self.path in ('/redirect', '/secure'):
                self.send_response(302)
                self.send_header('Location', '/index' if self.path == '/redirect' else 'https://example.com/')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path == '/slow':
                time.sleep(.5)
            data = (b'<title>Next live page</title><h1>Next page</h1><p>The live link worked.</p><a href="/index">Back to the first page</a>'
                    if self.path == '/next' else INDEX)
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name


def screenshot(process, destination):
    # QMP over pipes avoids a local socket and leaves no listening endpoint.
    for command in ({'execute': 'qmp_capabilities'},
                    {'execute': 'screendump', 'arguments': {'filename': str(destination)}}):
        process.stdin.write(json.dumps(command).encode() + b'\n')
        process.stdin.flush()
        deadline = time.monotonic() + 5
        received = b''
        while time.monotonic() < deadline:
            ready, _, _ = select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            data = os.read(process.stdout.fileno(), 65536)
            if not data:
                raise AssertionError('QEMU exited during screenshot')
            received += data
            if b'"error"' in received:
                raise AssertionError(f'QMP screenshot failed: {received!r}')
            if b'"return"' in received:
                break
        else:
            raise AssertionError('QMP screenshot timed out')
        if b'"return"' not in received:
            raise AssertionError('QMP screenshot timed out')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build.resolve()
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-browser-'))
    print(f'Browser QEMU evidence: {directory}', flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie',
                        '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float',
                        '-I', str(ROOT / 'src'), f'-DHTTP_PORT="{server.server_port}"', '-c',
                        str(ROOT / 'tests/browser_guest.c'), '-o', str(directory / 'guest.o')], check=True)
        subprocess.run(['nasm', '-f', 'elf', '-Dkmain=browser_guest', '-p', str(build / 'layout.inc'),
                        str(ROOT / 'src/kernel_entry.asm'), '-o', str(directory / 'entry.o')], check=True)
        objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel_entry.o']
        subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'),
                        str(directory / 'entry.o'), str(directory / 'guest.o'), *objects], check=True)
        subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'), str(directory / 'kernel.bin')], check=True)
        c = constants()
        disk = bytearray(c['DISK_SECTORS'] * 512)
        disk[:512] = (build / 'boot.bin').read_bytes()
        install_kernel(disk, (directory / 'kernel.bin').read_bytes(), c)
        image = directory / 'disk.img'
        image.write_bytes(disk)
        log = directory / 'serial.log'
        with (directory / 'stderr.log').open('w') as errors:
            process = subprocess.Popen(['qemu-system-i386', '-m', '32M', '-vga', 'std',
                                        '-drive', f'file={image},format=raw,index=0,if=floppy',
                                        '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0',
                                        '-serial', f'file:{log}', '-qmp', 'stdio',
                                        '-display', 'none', '-monitor', 'none', '-no-reboot'], stderr=errors,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    text = log.read_text() if log.exists() else ''
                    if 'BROWSER-QEMU-PASS' in text:
                        screenshot(process, directory / 'browser.ppm')
                        (directory / 'requests.json').write_text(json.dumps(Fixture.paths, indent=2))
                        assert all(path in Fixture.paths for path in ('/index', '/next', '/secure', '/slow'))
                        print(text, flush=True)
                        print('Actual HTTP requests:', ', '.join(Fixture.paths), flush=True)
                        print('Screenshot:', directory / 'browser.ppm', flush=True)
                        return
                    if 'BROWSER-FAIL' in text or 'PANIC' in text or process.poll() is not None:
                        break
                    time.sleep(.1)
                raise AssertionError(f'Browser guest failed:\n{text}\n{(directory / "stderr.log").read_text()}')
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
