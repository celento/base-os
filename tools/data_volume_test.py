"""Real-QEMU persistent-data checks using only disposable test images.

Build first, then: python3 tools/data_volume_test.py build [--keep]
Never opens build/baseos.img or build/baseos-data.img. No CPU fault injection.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from init_data import initialize
from layout import constants
from update_image import install_kernel
import volume

C = constants()
ROOT = Path(__file__).resolve().parents[1]
LARGE_SIZE = 2097152
HOST_SIZE = 262177
LEGACY_SIZE = 12017
FLOPPY_SIZE = 13001
TOTAL_SIZE = 8385024


def pattern(length, seed):
    return bytes((i * 37 + (i >> 16) + seed) & 255 for i in range(length))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tool(name):
    cross = 'x86_64-elf-' + name
    return cross if shutil.which(cross) else name


def build_fixture(build, directory, mode):
    target = directory / f'guest-{mode}'
    subprocess.run([tool('gcc'), '-std=gnu11', '-O2', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-msoft-float',
                    '-I', str(ROOT / 'src'), f'-DDATA_VOLUME_MODE={mode}',
                    '-c', str(ROOT / 'tests/data_volume_guest.c'),
                    '-o', str(target.with_suffix('.o'))], check=True)
    entry = directory / 'data_volume_entry.o'
    if not entry.exists():
        subprocess.run(['nasm', '-f', 'elf', '-Dkmain=data_volume_guest',
                        '-p', str(build / 'layout.inc'), str(ROOT / 'src/kernel_entry.asm'),
                        '-o', str(entry)], check=True)
    objects = [str(entry)] + [str(p) for p in sorted(build.glob('*.o'))
                              if p.name != 'kernel_entry.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib',
                    '-m', 'elf_i386', '-z', 'noexecstack', '-o',
                    str(target.with_suffix('.elf')), *objects,
                    str(target.with_suffix('.o'))], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(target.with_suffix('.elf')),
                    str(target.with_suffix('.bin'))], check=True)
    disk = bytearray(C['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes()
    # Ordinary build artifacts supply the base; only the disposable test image
    # gets a guest entry point. The user's persistent images are never read.
    install_kernel(disk, (build / 'kernel.bin').read_bytes(), C)
    install_kernel(disk, target.with_suffix('.bin').read_bytes(), C)
    return disk


def legacy_snapshot(disk):
    """Install a known, valid v3 snapshot using the host format's real codec."""
    payload = bytearray()
    for ident, name, content in ((0, '', b''), (1, 'old.bin', pattern(LEGACY_SIZE, 41))):
        payload += struct.pack('<HhBBHI24sI', ident, 0 if ident else -1,
                               int(ident == 0), 0, 0, len(content),
                               name.encode('ascii'), 1234 if ident else 0)
        payload += content
    header = struct.pack('<6I', volume.MAGIC, 3, 2, len(payload), zlib.crc32(payload), 7)
    header += struct.pack('<I', zlib.crc32(header))
    start = C['FS_DISK_LBA'] * 512
    disk[start:start + 512] = header.ljust(512, b'\0')
    disk[start + 512:start + 512 + len(payload)] = payload
    slot, generation, nodes = volume.load(disk)
    assert slot == 0 and generation == 7
    assert nodes[volume.resolve(nodes, '/old.bin')]['data'] == pattern(LEGACY_SIZE, 41)
    return disk


def run(floppy, data, directory, label, expected, seconds=90):
    log = directory / (label + '.log')
    errors = directory / (label + '.stderr')
    log.write_text('')
    command = ['qemu-system-i386', '-m', '64M', '-vga', 'std', '-boot', 'a',
               '-drive', f'file={floppy},format=raw,index=0,if=floppy',
               '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none',
               '-no-reboot']
    if data is not None:
        command += ['-drive', f'file={data},format=raw,index=0,if=ide,cache=writeback']
    started = time.monotonic()
    with errors.open('w') as output:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=output)
        try:
            deadline = started + seconds
            while time.monotonic() < deadline:
                text = log.read_text()
                if expected in text:
                    break
                if 'PANIC:' in text or process.poll() is not None:
                    raise AssertionError(f'{label}: {expected} missing\n{text}\n{errors.read_text()}')
                time.sleep(.1)
            else:
                raise AssertionError(f'{label}: timeout waiting for {expected}\n'
                                     f'{log.read_text()}\n{errors.read_text()}')
        finally:
            # A complete process exit closes both drives and their writeback
            # caches before the next boot or the host import/export process.
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError(f'{label}: QEMU failed to shut down normally')
    print(f'{label}: {expected} ({time.monotonic() - started:.1f}s)', flush=True)
    return log.read_text()


def cli(image, *args):
    subprocess.run([sys.executable, str(ROOT / 'tools/volume.py'), str(image),
                    *map(str, args)], check=True)


def export_equals(image, source, destination, expected):
    cli(image, 'export', source, destination)
    assert destination.read_bytes() == expected, f'export mismatch: {source}'


def snapshots(image, version):
    raw = image.read_bytes()
    layout = volume.disk_layout(raw)
    valid = [volume.decode(raw, slot) for slot in (0, 1)]
    for slot, decoded in enumerate(valid):
        if decoded is not None:
            assert struct.unpack_from('<I', raw, layout.lbas[slot] * 512 + 4)[0] == version
    assert any(valid), f'no valid v{version} snapshot'
    return valid


def main(build, keep):
    directory = Path(tempfile.mkdtemp(prefix='baseos-data-volume-'))
    print(f'QEMU logs and disposable test disks: {directory}', flush=True)
    succeeded = False
    try:
        floppy = directory / 'large-boot.img'
        floppy.write_bytes(build_fixture(build, directory, 1))
        floppy_before = digest(floppy)
        data = directory / 'large-data.img'
        assert initialize(data)
        marker = data.read_bytes()[:512]
        run(floppy, data, directory, 'large-write', 'DATA-LARGE-WRITE-PASS')
        export_equals(data, '/large.bin', directory / 'large-first.bin', pattern(LARGE_SIZE, 17))
        run(floppy, data, directory, 'large-restart', 'DATA-LARGE-RESTART-PASS')
        assert all(snapshots(data, 4)), 'large save did not alternate snapshots'
        host = directory / 'host-input.bin'
        host.write_bytes(pattern(HOST_SIZE, 93))
        cli(data, 'import', host, '/host.bin')
        text = run(floppy, data, directory, 'host-import-edit', 'DATA-HOST-EDIT-PASS')
        assert 'DATA-HOST-IMPORT-PASS' in text
        export_equals(data, '/Roundtrip/edited.bin', directory / 'host-edited.bin',
                      bytes(b ^ 0xA5 for b in pattern(HOST_SIZE, 93)))
        export_equals(data, '/Roundtrip/large.bin copy', directory / 'large-copy.bin', pattern(LARGE_SIZE, 17))
        data_before = digest(data)
        run(floppy, data, directory, 'host-edit-restart', 'DATA-HOST-RESTART-PASS')
        assert digest(data) == data_before, 'read-only verification changed data disk'
        assert digest(floppy) == floppy_before, 'data-volume work changed boot floppy'
        assert data.read_bytes()[:512] == marker
        assert data.read_bytes()[-512:] == bytes(512), 'reserved final data sector changed'

        floppy = directory / 'full-boot.img'
        floppy.write_bytes(build_fixture(build, directory, 5))
        floppy_before = digest(floppy)
        data = directory / 'full-data.img'
        assert initialize(data)
        run(floppy, data, directory, 'full-write', 'DATA-FULL-WRITE-PASS')
        original_slot = snapshots(data, 4)[0]
        assert original_slot and sum(len(n['data']) for n in original_slot[1].values()) == TOTAL_SIZE
        for i in range(4):
            length = LARGE_SIZE if i < 3 else TOTAL_SIZE - 3 * LARGE_SIZE
            export_equals(data, f'/full{i}.bin', directory / f'full-first-{i}.bin',
                          pattern(length, 101 + i * 7))
        text = run(floppy, data, directory, 'full-restart-refill', 'DATA-FULL-REFILL-PASS')
        assert 'DATA-FULL-RESTART-PASS' in text
        full_slots = snapshots(data, 4)
        assert all(full_slots), 'full-capacity commits did not reach both slots'
        assert full_slots[0] == original_slot, 'refill changed previous full snapshot'
        for generation, nodes in full_slots:
            assert sum(len(n['data']) for n in nodes.values()) == TOTAL_SIZE
            assert all(len(n['data']) <= LARGE_SIZE for n in nodes.values())
        export_equals(data, '/full0.bin', directory / 'full-shrunken.bin', b'small')
        export_equals(data, '/reclaimed.bin', directory / 'full-refilled.bin',
                      pattern(LARGE_SIZE - 5, 211))
        data_before = digest(data)
        run(floppy, data, directory, 'full-refill-restart', 'DATA-FULL-REFILL-RESTART-PASS')
        assert digest(data) == data_before, 'full-volume read/rejections changed image'
        assert digest(floppy) == floppy_before, 'full-volume work changed boot floppy'
        assert data.read_bytes()[:512] == volume.data_marker()
        assert data.read_bytes()[-512:] == bytes(512), 'full-volume writes reached reserved sector'

        floppy = directory / 'migration-boot.img'
        floppy.write_bytes(legacy_snapshot(build_fixture(build, directory, 2)))
        floppy_before = digest(floppy)
        data = directory / 'migration-data.img'
        assert initialize(data)
        run(floppy, data, directory, 'migration-write', 'DATA-MIGRATION-WRITE-PASS')
        assert digest(floppy) == floppy_before, 'migration changed original floppy bytes'
        snapshots(data, 4)
        export_equals(data, '/old.bin', directory / 'migrated.bin', pattern(LEGACY_SIZE, 41))
        data_before = digest(data)
        run(floppy, data, directory, 'migration-restart', 'DATA-MIGRATION-RESTART-PASS')
        assert digest(floppy) == floppy_before, 'migration reboot changed original floppy bytes'
        assert digest(data) == data_before, 'migration verification changed data disk'

        floppy = directory / 'fallback-boot.img'
        floppy.write_bytes(build_fixture(build, directory, 3))
        run(floppy, None, directory, 'floppy-write', 'DATA-FLOPPY-WRITE-PASS')
        snapshots(floppy, 3)
        export_equals(floppy, '/floppy.bin', directory / 'floppy-export.bin', pattern(FLOPPY_SIZE, 71))
        floppy_before = digest(floppy)
        run(floppy, None, directory, 'floppy-restart', 'DATA-FLOPPY-RESTART-PASS')
        assert digest(floppy) == floppy_before, 'floppy read-only verification changed image'

        protected = legacy_snapshot(build_fixture(build, directory, 4))
        for label, content in (('unmarked-blank', bytes(C['DATA_DISK_SECTORS'] * 512)),
                               ('unmarked-populated', b'ORDINARY FOREIGN DATA\0'.ljust(512, b'\0')
                                + bytes(C['DATA_DISK_SECTORS'] * 512 - 1024)
                                + bytes(range(256)) * 2)):
            floppy = directory / (label + '-boot.img')
            floppy.write_bytes(protected)
            data = directory / (label + '-data.img')
            data.write_bytes(content)
            floppy_before, data_before = digest(floppy), digest(data)
            run(floppy, data, directory, label, 'DATA-UNMARKED-PROTECTED-PASS')
            assert digest(floppy) == floppy_before, f'{label}: protected floppy changed'
            assert digest(data) == data_before, f'{label}: unknown IDE changed'
        succeeded = True
        print('All real-QEMU data-volume checks passed: 2 MiB, full capacity/reclaim, restart, host exchange, '
              'v3 migration, floppy fallback, and protected unmarked media.', flush=True)
    finally:
        # Failed runs retain exact evidence without risking persistent images.
        if not keep and succeeded:
            shutil.rmtree(directory)
        else:
            print(f'Retained logs and disposable images: {directory}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    main(args.build.resolve(), args.keep)
