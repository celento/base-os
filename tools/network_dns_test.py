"""Deterministic real-NIC DNS check using a local QEMU Ethernet peer.

This fixture replaces the external network with a loopback-only Ethernet
responder. It tests the RTL8139/ARP/IPv4/UDP/DNS path without public DNS access.
It is deliberately separate from network_test.py's real user-NAT HTTP tests.
"""
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from layout import constants
from update_image import install_kernel
from network_test import ROOT, tool

MAC = bytes.fromhex('52550a000203')
DNS = bytes([10, 0, 2, 3])


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def answer(frame):
    kind = frame[12:14]
    if kind == b'\x08\x06':
        packet = frame[14:42]
        if len(packet) != 28 or packet[6:8] != b'\0\x01' or packet[24:28] != DNS:
            return None
        return (frame[6:12] + MAC + kind + packet[:6] + b'\0\x02' + MAC + DNS +
                packet[8:14] + packet[14:18])
    if kind != b'\x08\x00':
        return None
    ip = frame[14:]
    header = (ip[0] & 15) * 4
    if len(ip) < header + 8 or ip[9] != 17 or ip[16:20] != DNS:
        return None
    udp = ip[header:struct.unpack_from('!H', ip, 2)[0]]
    if udp[2:4] != b'\0\x35':
        return None
    query = udp[8:]
    offset = 12
    labels = []
    while query[offset]:
        size = query[offset]
        offset += 1
        labels.append(query[offset:offset + size].decode('ascii'))
        offset += size
    question = query[12:offset + 5]
    name = '.'.join(labels)
    assert name in ('fixture.test', 'alias.fixture.test'), name
    result = bytearray(query[:2] + b'\x81\x80\0\x01' +
                       (b'\0\x02' if name.startswith('alias.') else b'\0\x01') + b'\0\0\0\0' + question)
    a_name = 12
    if name.startswith('alias.'):
        target = b'\x07fixture\x04test\0'
        a_name = len(result) + 12
        result += b'\xc0\x0c\0\x05\0\x01\0\0\0\x3c' + struct.pack('!H', len(target)) + target
    result += struct.pack('!HHHIH', 0xc000 | a_name, 1, 1, 60, 4) + bytes([10, 0, 2, 2])
    payload = b'\0\x35' + udp[:2] + struct.pack('!HH', 8 + len(result), 0) + result
    header = bytearray(b'\x45\0' + struct.pack('!H', 20 + len(payload)) + b'\0\x01\0\0\x40\x11\0\0' + DNS + ip[12:16])
    struct.pack_into('!H', header, 10, checksum(header))
    return frame[6:12] + MAC + kind + header + payload


def exact(connection, count):
    data = bytearray()
    while len(data) < count:
        part = connection.recv(count - len(data))
        if not part:
            raise EOFError
        data += part
    return data


def peer(listener, stop, errors, frames):
    try:
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(.5)
            while not stop.is_set():
                try:
                    size = struct.unpack('!I', exact(connection, 4))[0]
                except socket.timeout:
                    continue
                frame = exact(connection, size)
                frames.append(bytes(frame))
                response = answer(frame)
                if response:
                    response += bytes(max(0, 60 - len(response)))
                    connection.sendall(struct.pack('!I', len(response)) + response)
    except (EOFError, ConnectionResetError):
        pass
    except Exception as error:
        errors.append(error)


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-network-dns-'))
    print(f'Controlled DNS evidence: {directory}', flush=True)
    subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie', '-fno-stack-protector',
                    '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(ROOT / 'src'),
                    '-c', str(ROOT / 'tests/network_dns_guest.c'), '-o', str(directory / 'guest.o')], check=True)
    subprocess.run(['nasm', '-f', 'elf', '-Dkmain=network_dns_guest', '-p', str(build / 'layout.inc'),
                    str(ROOT / 'src/kernel_entry.asm'), '-o', str(directory / 'entry.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel_entry.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386', '-z', 'noexecstack',
                    '-o', str(directory / 'kernel.elf'), str(directory / 'entry.o'), str(directory / 'guest.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'), str(directory / 'kernel.bin')], check=True)
    c = constants()
    disk = bytearray(c['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(disk, (directory / 'kernel.bin').read_bytes(), c)
    image = directory / 'disk.img'
    image.write_bytes(disk)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        stop = threading.Event()
        errors, frames = [], []
        worker = threading.Thread(target=peer, args=(listener, stop, errors, frames), daemon=True)
        worker.start()
        port = listener.getsockname()[1]
        log = directory / 'serial.log'
        with (directory / 'stderr.log').open('w') as stderr:
            process = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std', '-drive',
                                        f'file={image},format=raw,index=0,if=floppy',
                                        '-netdev', f'socket,id=net0,connect=127.0.0.1:{port}',
                                        '-device', 'rtl8139,netdev=net0', '-object',
                                        f'filter-dump,id=capture,netdev=net0,file={directory / "traffic.pcap"}',
                                        '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot'], stderr=stderr)
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    text = log.read_text() if log.exists() else ''
                    if 'NETWORK-DNS-QEMU-PASS' in text:
                        assert not errors, errors
                        print(text, flush=True)
                        return
                    if 'PANIC' in text or errors or process.poll() is not None:
                        break
                    time.sleep(.1)
                raise AssertionError(f'{log.read_text()}\n{errors}\n{(directory / "stderr.log").read_text()}')
            finally:
                stop.set()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
                worker.join(timeout=2)


if __name__ == '__main__':
    main()
