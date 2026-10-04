"""Normal native SDK expansion checks in disposable 64 MiB QEMU guests.
No malformed binaries, fault probes, or saved user disks are used.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib
from build_app import build as build_app, tool
from data_volume_test import run
from layout import constants
from make_stats_fixture import document
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]


def snapshot(files):
    data = bytearray(volume.DATA_LAYOUT.sectors * 512)
    data[:512] = volume.data_marker()
    nodes = [(0, -1, '', True, b''), (1, 0, 'Programs', True, b''),
             (2, 0, 'Documents', True, b'')]
    for parent, name, content in files:
        nodes.append((len(nodes), parent, name, False, content))
    payload = bytearray()
    for ident, parent, name, directory, content in nodes:
        payload += struct.pack('<HhBBHI24sI', ident, parent, directory, 0, 0,
                               len(content), name.encode('ascii'), 1234)
        payload += content
    header = struct.pack('<6I', volume.MAGIC, 4, len(nodes), len(payload),
                         zlib.crc32(payload), 1)
    header += struct.pack('<I', zlib.crc32(header))
    start = volume.DATA_FIRST_LBA * 512
    data[start:start + 512] = header.ljust(512, b'\0')
    data[start + 512:start + 512 + len(payload)] = payload
    assert volume.load(data)[1] == 1
    return data


def main(build, compile_only=False):
    directory = Path(tempfile.mkdtemp(prefix='baseos-native-sdk-'))
    print(f'Native SDK test files: {directory}', flush=True)
    files = []
    for source, name in [('tests/sdk_stream_app.c', 'stream.bex'),
                         ('tests/sdk_capacity_app.c', 'capacity.bex'),
                         ('examples/c/docstats.c', 'docstats.bex')]:
        output = directory / name
        build_app(ROOT / source, output)
        files.append((1, name, output.read_bytes()))
    # The exact largest valid BEX1 is an exit-only program plus application data.
    entry = b'\x31\xc0\x31\xdb\xcd\x80'
    largest = struct.pack('<4I', 0x31584542, 16, 49152, 0) + entry
    largest = largest.ljust(49152, b'\0')
    files += [(1, 'maximage.bex', largest), (2, 'stats-sample.txt', document()),
              (2, 'native-input.bin', bytes((i * 37 + (i >> 16) + 11) & 255
                                           for i in range(2097152)))]
    assert sum(document()) == 4874156
    (directory / 'data.img').write_bytes(snapshot(files))
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(ROOT),
                    '-I', str(ROOT / 'src'), '-I', str(build),
                    '-c', str(ROOT / 'tests/sdk_expanded_guest.c'),
                    '-o', str(directory / 'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib',
                    '-m', 'elf_i386', '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.bin')], check=True)
    c = constants()
    image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (directory / 'kernel.bin').read_bytes(), c)
    (directory / 'boot.img').write_bytes(image)
    if compile_only:
        print('Compiled guest and valid disposable IDE snapshot; QEMU not started.', flush=True)
        return
    run(directory / 'boot.img', directory / 'data.img', directory,
        'expanded', 'NATIVE-SDK-EXPANDED-PASS', seconds=180)
    run(directory / 'boot.img', directory / 'data.img', directory,
        'expanded-reboot', 'NATIVE-SDK-EXPANDED-REBOOT-PASS', seconds=120)
    _, _, nodes = volume.load((directory / 'data.img').read_bytes())
    for owner in (1, 2):
        content = nodes[volume.resolve(nodes, f'/Documents/native-{owner}.bin')]['data']
        assert len(content) == 32768
        assert struct.unpack_from('<4I', content) == (owner, 2097152, 267386880, 512)
    print('Native SDK: 48 KiB images, concurrent 2 MiB streams, EOF, 32 KiB durable '
          'replacement, DocStats, legacy reset and full-capacity reboot passed.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    main(args.build.resolve(), args.compile_only)
