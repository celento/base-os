"""Real RTL8139 + QEMU user-network integration, with a loopback HTTP fixture.
Never reads, boots, or edits the user's persistent disk image.
Usage: python3 tools/network_test.py build [--public-dns]
"""
import argparse
import http.server
import pathlib
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from layout import constants
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            if self.path == '/chunked':
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.send_header('Content-Type', 'text/plain')
                self.end_headers()
                for chunk in (b'Hello ', b'chunked ', b'world!\n'):
                    self.wfile.write(f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n')
                    self.wfile.flush()
                self.wfile.write(b'0\r\nX-Test: finished\r\n\r\n')
            elif self.path == '/close':
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain')
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(b'Close-delimited response.\n')
                self.close_connection = True
            elif self.path == '/broken':
                self.send_response(200)
                self.send_header('Content-Length', '100')
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(b'short')
                self.close_connection = True
            else:
                data = (bytes(ord('a') + i % 26 for i in range(20000))
                        if self.path == '/large' else b'BaseOS real HTTP works.\n')
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Content-Type', 'text/plain')
                self.end_headers()
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--public-dns', action='store_true', help='also resolve example.com through QEMU DNS')
    args = parser.parse_args()
    build = args.build.resolve()
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-network-'))
    print(f'Network QEMU evidence: {directory}', flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        flags = [f'-DHTTP_PORT="{server.server_port}"']
        if args.public_dns:
            flags += ['-DTEST_PUBLIC_DNS']
        subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie',
                        '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx',
                        '-msoft-float', '-I', str(ROOT / 'src'), *flags, '-c',
                        str(ROOT / 'tests/network_guest.c'), '-o', str(directory / 'guest.o')], check=True)
        subprocess.run(['nasm', '-f', 'elf', '-Dkmain=network_guest', '-p', str(build / 'layout.inc'),
                        str(ROOT / 'src/kernel_entry.asm'), '-o', str(directory / 'entry.o')], check=True)
        objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel_entry.o']
        subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'),
                        str(directory / 'entry.o'), str(directory / 'guest.o'), *objects], check=True)
        subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'),
                        str(directory / 'kernel.bin')], check=True)
        c = constants()
        disk = bytearray(c['DISK_SECTORS'] * 512)
        disk[:512] = (build / 'boot.bin').read_bytes()
        install_kernel(disk, (directory / 'kernel.bin').read_bytes(), c)
        image = directory / 'disk.img'
        image.write_bytes(disk)
        log = directory / 'serial.log'
        with (directory / 'stderr.log').open('w') as errors:
            process = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std',
                                        '-drive', f'file={image},format=raw,index=0,if=floppy',
                                        '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0',
                                        '-object', f'filter-dump,id=capture,netdev=net0,file={directory / "traffic.pcap"}',
                                        '-serial', f'file:{log}',
                                        '-display', 'none', '-monitor', 'none', '-no-reboot'], stderr=errors)
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    text = log.read_text() if log.exists() else ''
                    if 'NETWORK-QEMU-PASS' in text:
                        print(text, flush=True)
                        return
                    if 'NETWORK-FAIL' in text or 'PANIC' in text or process.poll() is not None:
                        break
                    time.sleep(.1)
                raise AssertionError(f'Network guest failed:\n{log.read_text()}\n'
                                     f'{(directory / "stderr.log").read_text()}')
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
