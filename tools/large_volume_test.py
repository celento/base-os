"""Ordinary opt-in large-volume guest checks using only disposable images.

Build first, then: python3 tools/large_volume_test.py build --keep
--compile-only prepares valid test images and native programs without QEMU.
Boots run strictly serially. No CPU/memory fault probes or saved images are used.
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
from build_app import build as build_app
from data_volume_test import tool
from init_data import initialize
from layout import constants
from migrate_volume import migrate
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]
C = constants()


def pattern(length):
    result = bytearray()
    for offset in range(0, length, 65536):
        block = bytes((i * 37 + (offset >> 16) + 19) & 255 for i in range(256)) * 256
        result.extend(block[:min(65536, length - offset)])
    return bytes(result)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_fixture(build, directory, mode):
    label = f'guest-{mode}'
    source = ROOT / 'tests' / ('large_volume_ui_guest.c' if mode == 'ui' else 'large_volume_guest.c')
    obj, elf, binary = [directory / (label + extension) for extension in ('.o', '.elf', '.bin')]
    flags = [] if mode == 'ui' else [f'-DLARGE_VOLUME_MODE={mode}']
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32', '-fno-pie',
                    '-fno-pic', '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-msoft-float', '-I', str(ROOT), '-I', str(ROOT / 'src'), '-I', str(build),
                    *flags, '-c', str(source), '-o', str(obj)], check=True)
    if mode == 'ui':
        objects = [str(p) for p in sorted(build.glob('*.o')) if p.name != 'kernel.o']
    else:
        entry = directory / 'large_entry.o'
        if not entry.exists():
            subprocess.run(['nasm', '-f', 'elf', '-Dkmain=large_volume_guest', '-p',
                            str(build / 'layout.inc'), str(ROOT / 'src/kernel_entry.asm'),
                            '-o', str(entry)], check=True)
        objects = [str(entry)] + [str(p) for p in sorted(build.glob('*.o')) if p.name != 'kernel_entry.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(elf), *objects, str(obj)], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
    raw = bytearray(C['DISK_SECTORS'] * 512)
    raw[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(raw, binary.read_bytes(), C)
    return raw


def node(name='', parent=-1, directory=1, app=0, data=b'', modified=0):
    return dict(name=name, parent=parent, directory=directory, app=app, data=data, modified=modified)


def place_snapshot(raw, nodes, layout):
    header, payload = volume.encode_snapshot(nodes, layout, 7)
    start = layout.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    assert volume.decode(raw, 0, layout)[1] == nodes
    return raw


def run(floppy, data, directory, label, expected, *, memory='128M', seconds=180):
    log, errors = directory / (label + '.log'), directory / (label + '.stderr')
    log.write_text('')
    command = ['qemu-system-i386', '-m', memory, '-vga', 'std', '-boot', 'a',
               '-drive', f'file={floppy},format=raw,index=0,if=floppy',
               '-drive', f'file={data},format=raw,index=0,if=ide,cache=writeback',
               '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot']
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
                    raise AssertionError(f'{label}: {expected} absent\n{text}\n{errors.read_text()}')
                time.sleep(.1)
            else:
                raise AssertionError(f'{label}: deadline waiting for {expected}\n{log.read_text()}')
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
                raise AssertionError(f'{label}: emulator did not shut down normally')
    print(f'{label}: {expected} ({time.monotonic() - started:.1f}s)', flush=True)
    return log.read_text()


def validate_data(path, expected=None):
    raw = path.read_bytes()
    layout = volume.disk_layout(raw)
    assert layout == volume.LARGE_DATA_LAYOUT
    assert raw[:512] == volume.data_marker('large') and raw[-512:] == bytes(512)
    decoded = [volume.decode(raw, slot) for slot in (0, 1)]
    assert any(decoded)
    for slot, value in enumerate(decoded):
        if value:
            start = layout.lbas[slot] * 512
            magic, version, count, length, crc, _, hcrc = struct.unpack_from('<7I', raw, start)
            assert magic == volume.MAGIC and version == 5 and count == len(value[1])
            import zlib
            assert zlib.crc32(raw[start:start + 24]) == hcrc
            assert zlib.crc32(raw[start + 512:start + 512 + length]) == crc
    nodes = volume.load(raw)[2]
    if expected is not None:
        assert nodes == expected
    return nodes


def main(build, keep=False, compile_only=False):
    directory = Path(tempfile.mkdtemp(prefix='baseos-large-volume-'))
    print(f'Large-volume evidence: {directory}', flush=True)
    passed = False
    try:
        boot = {}
        for mode in (1, 2, 3, 'ui'):
            boot[mode] = directory / f'boot-{mode}.img'
            boot[mode].write_bytes(build_fixture(build, directory, mode))
        legacy = {0: node(), 1: node('legacy', 0, 0, data=pattern(12345), modified=777),
                  2: node('App', 0, 0, 1, modified=999)}
        for mode in (2, 3):
            boot[mode].write_bytes(place_snapshot(bytearray(boot[mode].read_bytes()), legacy, volume.FLOPPY_LAYOUT))
        full, migration, default, converted, desktop = [directory / name for name in
            ('full-data.img', 'migration-data.img', 'default-source.img', 'converted-data.img', 'desktop-data.img')]
        initialize(full, profile='large'); initialize(migration, profile='large'); initialize(default)
        default.write_bytes(place_snapshot(bytearray(default.read_bytes()), legacy, volume.DATA_LAYOUT))
        source_hash = digest(default); migrate(default, converted)
        assert digest(default) == source_hash
        app = directory / 'large-stream.bex'; build_app(ROOT / 'tests/large_stream_app.c', app)
        initialize(desktop, profile='large')
        desktop_nodes = {0: node(), 1: node('Documents', 0), 2: node('Programs', 0),
                         3: node('input-large.bin', 1, 0, data=pattern(16777216), modified=1234),
                         4: node('large-stream.bex', 2, 0, data=app.read_bytes(), modified=1234),
                         5: node('trash', 0), 6: node('prefs', 0), 7: node('Pictures', 0),
                         8: node('docs', 0)}
        desktop.write_bytes(place_snapshot(bytearray(desktop.read_bytes()), desktop_nodes, volume.LARGE_DATA_LAYOUT))
        if compile_only:
            print('Prepared four guest kernels and valid disposable images; QEMU not started.', flush=True)
            return
        boot_hashes = {mode: digest(path) for mode, path in boot.items()}
        run(boot[1], full, directory, 'full-create', 'LARGE-FULL-WRITE-PASS')
        nodes = validate_data(full)
        assert len(nodes) == 256 and sum(len(n['data']) for n in nodes.values()) == 33543168
        for name in ('maximum', 'remainder'):
            content = nodes[volume.resolve(nodes, '/' + name)]['data']; assert content == pattern(len(content))
        first = volume.decode(full.read_bytes(), 0)
        run(boot[1], full, directory, 'full-reboot-save', 'LARGE-FULL-REBOOT-SAVE-PASS')
        assert volume.decode(full.read_bytes(), 0) == first
        nodes = validate_data(full)
        assert nodes[255]['app'] == 1
        before = digest(full)
        run(boot[1], full, directory, 'full-readonly-reboot', 'LARGE-FULL-READONLY-REBOOT-PASS')
        assert digest(full) == before
        run(boot[2], full, directory, 'low-ram-protected', 'LARGE-LOW-RAM-PASS', memory='64M')
        assert digest(full) == before
        run(boot[3], migration, directory, 'legacy-migrate', 'LARGE-MIGRATION-PASS')
        validate_data(migration, legacy)
        before = digest(migration)
        run(boot[3], migration, directory, 'legacy-migrate-reboot', 'LARGE-MIGRATION-PASS')
        assert digest(migration) == before
        run(boot[3], converted, directory, 'explicit-v4-conversion', 'LARGE-MIGRATION-PASS')
        validate_data(converted, legacy); assert digest(default) == source_hash
        run(boot['ui'], desktop, directory, 'desktop-native', 'LARGE-DESKTOP-NATIVE-PASS')
        run(boot['ui'], desktop, directory, 'desktop-native-reboot', 'LARGE-DESKTOP-NATIVE-REBOOT-PASS')
        nodes = validate_data(desktop)
        assert nodes[volume.resolve(nodes, '/Documents/input-large.bin')]['data'] == pattern(16777216)
        assert nodes[volume.resolve(nodes, '/Documents/large-stream.bin')]['data'] == struct.pack('<4I', 16777216, 2139095040, 4096, 0x4c415247)
        for mode, path in boot.items():
            assert digest(path) == boot_hashes[mode], f'boot floppy {mode} changed'
        passed = True
        print('Large profile: exact full snapshots, low-RAM refusal, migration, desktop/session and native 16 MiB stream passed.', flush=True)
    finally:
        if passed and not keep:
            shutil.rmtree(directory)
        else:
            print(f'Retained evidence: {directory}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    main(args.build.resolve(), args.keep, args.compile_only)
