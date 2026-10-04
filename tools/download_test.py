"""Real RTL8139 background downloads and offline Browser saves in QEMU.
Only disposable floppy/data images and a loopback HTTP server are used.
Usage: python3 tools/download_test.py build
"""
import argparse
import array
import hashlib
import http.server
import json
import pathlib
import shutil
import subprocess
import tempfile
import threading
import time
import wave
from browser_test import screenshot
from init_data import initialize
from layout import constants
from update_image import install_kernel
import volume

ROOT = pathlib.Path(__file__).resolve().parents[1]
PAGE = (b'<!doctype html><title>Saved HTTP page</title><h1>Offline page</h1>'
        b'<p>Original bytes &amp; markup.</p><script>not displayed</script>\n')
BINARY = bytes((i * 37 + (i >> 16) + 91) & 255 for i in range(2097153))


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    paths = []

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.paths.append(self.path)
        try:
            status = 302 if self.path == '/redirect' else 404 if self.path == '/missing' else 200
            data = (PAGE if self.path == '/page' else BINARY[:20037] if self.path == '/medium'
                    else BINARY if self.path == '/oversized' else b'Not found' if status == 404
                    else b'' if status == 302 else BINARY[:2097152])
            self.send_response(status)
            self.send_header('Content-Length', str(100 if self.path == '/broken' else len(data)))
            self.send_header('Content-Type', 'text/html' if self.path == '/page' else 'application/octet-stream')
            if status == 302:
                self.send_header('Location', 'https://example.com/never-follow')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            if self.path == '/broken':
                self.wfile.write(b'short')
            elif self.path in ('/slow', '/binary'):
                for offset in range(0, len(data), 8192):
                    self.wfile.write(data[offset:offset+8192])
                    self.wfile.flush()
                    time.sleep(.05 if self.path == '/slow' else .01)
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name


def build_fixture(build, directory, port):
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-mno-sse',
                    '-mno-mmx', '-msoft-float', '-I', str(ROOT), '-I', str(ROOT / 'src'),
                    '-I', str(build), f'-DHTTP_PORT="{port}"', '-c',
                    str(ROOT / 'tests/download_guest.c'), '-o', str(directory / 'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.bin')], check=True)
    c = constants()
    image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (directory / 'kernel.bin').read_bytes(), c)
    (directory / 'disk.img').write_bytes(image)
    initialize(directory / 'data.img')


def boot(directory, label, marker, capture=False):
    log = directory / f'{label}.log'
    audio = directory / f'{label}.wav'
    with (directory / f'{label}.stderr').open('w') as errors:
        process = subprocess.Popen([
            'qemu-system-i386', '-m', '64M', '-vga', 'std', '-boot', 'a',
            '-drive', f'file={directory / "disk.img"},format=raw,index=0,if=floppy',
            '-drive', f'file={directory / "data.img"},format=raw,index=0,if=ide,cache=writeback',
            '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0',
            '-object', f'filter-dump,id=capture,netdev=net0,file={directory / (label+".pcap")}',
            '-audiodev', f'wav,id=test,path={audio},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=test', '-serial', f'file:{log}', '-qmp', 'stdio',
            '-display', 'none', '-monitor', 'none', '-no-reboot'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0)
        try:
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                text = log.read_text() if log.exists() else ''
                if marker in text:
                    if capture:
                        screenshot(process, directory / 'downloads.ppm')
                    print(text, flush=True)
                    return audio
                if 'DOWNLOAD-FAIL ' in text or 'PANIC' in text or process.poll() is not None:
                    raise AssertionError(text)
                time.sleep(.1)
            raise AssertionError('Download QEMU timeout:\n' + log.read_text())
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-download-'))
    print(f'Download QEMU evidence: {directory}', flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        build_fixture(args.build.resolve(), directory, server.server_port)
        audio = boot(directory, 'download', 'DOWNLOAD-QEMU-PASS', capture=True)
        boot(directory, 'reboot', 'DOWNLOAD-REBOOT-PASS')
        _, _, nodes = volume.load((directory / 'data.img').read_bytes())
        saved = nodes[volume.resolve(nodes, '/binary.bin')]['data']
        assert saved == BINARY[:2097152]
        assert nodes[volume.resolve(nodes, '/medium.bin')]['data'] == BINARY[:20037]
        for path in ('/Downloads/page.html', '/Downloads/page-2.html'):
            assert nodes[volume.resolve(nodes, path)]['data'] == PAGE
        for path in ('/cancel.bin', '/too-large.bin', '/incomplete.bin', '/redirect.bin', '/missing.bin'):
            try:
                volume.resolve(nodes, path)
            except ValueError:
                pass
            else:
                raise AssertionError('Failed request left a file: '+path)
        with wave.open(str(audio), 'rb') as wav:
            pcm = array.array('h', wav.readframes(wav.getnframes()))
        assert pcm and max(map(abs, pcm)) > 500, 'Captured SB16 audio is missing'
        assert all(path in Fixture.paths for path in ('/medium', '/slow', '/oversized', '/broken', '/redirect', '/missing', '/binary', '/page'))
        (directory / 'requests.json').write_text(json.dumps(Fixture.paths, indent=2))
        (directory / 'binary.sha256').write_text(hashlib.sha256(saved).hexdigest()+'\n')
        print('Verified exact 2 MiB binary + offline page reboot, cancellation, error preservation, concurrent Editor/native/audio, and framebuffer capture.')
        print('Binary SHA-256:', hashlib.sha256(saved).hexdigest())
        print('Desktop screenshot:', directory / 'downloads.ppm')
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
