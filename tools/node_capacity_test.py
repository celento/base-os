"""Ordinary QEMU node-capacity checks. Uses only new disposable images.

Build first, then: python3 tools/node_capacity_test.py build [--keep]
Does not read or write either saved build/*.img disk.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib
from data_volume_test import C, ROOT, tool, run, pattern, cli, export_equals, digest
from init_data import initialize
from update_image import install_kernel
import volume


def build_fixture(build, directory, mode):
    target = directory / f'node-guest-{mode}'
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(ROOT / 'src'),
                    f'-DNODE_TEST_MODE={mode}', '-c', str(ROOT / 'tests/node_capacity_guest.c'),
                    '-o', str(target.with_suffix('.o'))], check=True)
    entry = directory / 'node-entry.o'
    if not entry.exists():
        subprocess.run(['nasm', '-f', 'elf', '-Dkmain=node_capacity_guest',
                        '-p', str(build / 'layout.inc'), str(ROOT / 'src/kernel_entry.asm'),
                        '-o', str(entry)], check=True)
    objects = [str(entry)] + [str(p) for p in sorted(build.glob('*.o')) if p.name != 'kernel_entry.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(target.with_suffix('.elf')), *objects,
                    str(target.with_suffix('.o'))], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(target.with_suffix('.elf')),
                    str(target.with_suffix('.bin'))], check=True)
    disk = bytearray(C['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(disk, target.with_suffix('.bin').read_bytes(), C)
    return disk


def old_full_data():
    image = bytearray(volume.DATA_LAYOUT.sectors * 512)
    image[:512] = volume.data_marker()
    payload = bytearray()
    for ident in range(64):
        name = '' if not ident else f'large{ident - 1}' if ident <= 4 else f'empty{ident}'
        length = volume.DATA_FILE_LIMIT if 1 <= ident <= 3 else 0
        if ident == 4:
            length = 8385024 - 3 * volume.DATA_FILE_LIMIT
        content = pattern(length, ident - 1)
        payload += struct.pack('<HhBBHI24sI', ident, 0 if ident else -1, int(not ident),
                               0, 0, length, name.encode(), 1234) + content
    assert len(payload) == volume.DATA_LAYOUT.payload_limit
    header = struct.pack('<6I', volume.MAGIC, 4, 64, len(payload), zlib.crc32(payload), 7)
    header += struct.pack('<I', zlib.crc32(header))
    start = volume.DATA_FIRST_LBA * 512
    image[start:start + 512] = header.ljust(512, b'\0')
    image[start + 512:start + 512 + len(payload)] = payload
    assert len(volume.load(image)[2]) == 64
    return image


def main(build, keep):
    directory = Path(tempfile.mkdtemp(prefix='baseos-node-capacity-'))
    print(f'QEMU logs and disposable test disks: {directory}', flush=True)
    success = False
    try:
        floppy, data = directory / 'nodes-boot.img', directory / 'nodes-data.img'
        floppy.write_bytes(build_fixture(build, directory, 1))
        before_floppy = digest(floppy)
        initialize(data)
        run(floppy, data, directory, 'nodes-write', 'NODE-256-WRITE-PASS')
        raw = data.read_bytes()
        _, _, nodes = volume.load(raw)
        assert len(nodes) == 256 and max(nodes) == 255
        assert volume.DATA_LAYOUT.capacity(len(nodes)) == 8377344
        before_data = digest(data)
        run(floppy, data, directory, 'nodes-restart', 'NODE-256-RESTART-PASS')
        assert digest(data) == before_data
        deep = '/abcdefghijklmnopqrstuvw' * 60
        export_equals(data, deep + '/moved.bin', directory / 'moved.bin', pattern(1214, 189))
        export_equals(data, deep + '/doc188.bin copy', directory / 'copy.bin', pattern(1213, 188))
        host = directory / 'host.bin'; host.write_bytes(pattern(1025, 211))
        cli(data, 'import', host, '/doc000.bin', '--replace')
        stage = directory / 'stage'; stage.write_bytes(b'H')
        cli(data, 'import', stage, '/stage', '--replace')
        export_equals(data, '/doc000.bin', directory / 'exported.bin', host.read_bytes())
        before_data = digest(data)
        run(floppy, data, directory, 'nodes-host-restart', 'NODE-HOST-EXCHANGE-RESTART-PASS')
        assert digest(data) == before_data and digest(floppy) == before_floppy
        assert all(volume.decode(data.read_bytes(), slot) for slot in (0, 1))
        assert data.read_bytes()[:512] == volume.data_marker()
        assert data.read_bytes()[-512:] == bytes(512)

        floppy, data = directory / 'old-full-boot.img', directory / 'old-full-data.img'
        floppy.write_bytes(build_fixture(build, directory, 2))
        before_floppy = digest(floppy)
        data.write_bytes(old_full_data())
        run(floppy, data, directory, 'old-full-expand', 'NODE-OLD-FULL-EXPAND-PASS')
        raw = data.read_bytes(); _, _, nodes = volume.load(raw)
        assert len(nodes) == 65 and sum(len(n['data']) for n in nodes.values()) == 8384984
        assert all(volume.decode(raw, slot) for slot in (0, 1))
        export_equals(data, '/large3', directory / 'old-full-after.bin', pattern(8385024 - 3 * volume.DATA_FILE_LIMIT - 40, 3))
        run(floppy, data, directory, 'old-full-reclaim', 'NODE-OLD-FULL-RESTART-RECLAIM-PASS')
        _, _, nodes = volume.load(data.read_bytes())
        assert len(nodes) == 64 and sum(len(n['data']) for n in nodes.values()) == 8385024
        export_equals(data, '/large3', directory / 'old-full-refilled.bin', pattern(8385024 - 3 * volume.DATA_FILE_LIMIT, 3))
        assert digest(floppy) == before_floppy
        assert data.read_bytes()[:512] == volume.data_marker()
        assert data.read_bytes()[-512:] == bytes(512)
        success = True
        print('256-node persistence, deep paths, binary exchange, full old v4 admission and reclaim passed.', flush=True)
    finally:
        if success and not keep:
            shutil.rmtree(directory)
        else:
            print(f'Retained logs and disposable images: {directory}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--keep', action='store_true')
    arguments = parser.parse_args()
    main(arguments.build.resolve(), arguments.keep)
