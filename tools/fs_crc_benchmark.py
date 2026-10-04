"""Serialized real-QEMU CRC comparison on identical disposable full volumes.

Build first, then use --baseline-ref to select the earlier filesystem source.
Both sources are compiled with the same production -Os flags and linked with
identical ordinary build objects. No build/*.img file is read or changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import tempfile
import time
import zlib

from data_volume_test import tool
from layout import constants
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]
C = constants()
PHASES = ('MOUNT', 'SAVE', 'REMOUNT')
FLAGS = ['-std=gnu11', '-ffreestanding', '-Os', '-g', '-Wall', '-Wextra', '-m32',
         '-nostdlib', '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
         '-mno-sse', '-mno-mmx', '-msoft-float']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def full_volume():
    pattern = bytes((i * 37 + (i >> 16) + 19) & 255 for i in range(2097152))
    payload = bytearray()
    for ident in range(256):
        size = (2097152 if ident < 4 else 8377344 - 3 * 2097152) if 1 <= ident <= 4 else 0
        payload += struct.pack('<HhBBHI24sI', ident, 0 if ident else -1, int(not ident),
                               0, 0, size, f'file{ident}'.encode() if ident else b'',
                               1234 if ident else 0)
        payload += pattern[:size]
    assert len(payload) == 8387584
    raw = bytearray(C['DATA_DISK_SECTORS'] * 512)
    raw[:512] = volume.data_marker()
    for slot, lba in enumerate((C['DATA_FIRST_LBA'], C['DATA_SECOND_LBA'])):
        header = struct.pack('<6I', volume.MAGIC, 4, 256, len(payload), zlib.crc32(payload), 17 + slot)
        header += struct.pack('<I', zlib.crc32(header))
        start = lba * 512
        raw[start:start + 28] = header
        raw[start + 512:start + 512 + len(payload)] = payload
        assert volume.decode(raw, slot) is not None
    return bytes(raw)


def build_variants(build, directory, baseline):
    before = subprocess.check_output(['git', '-C', str(ROOT), 'show', baseline + ':src/fs.c'])
    sources = {'before': before, 'after': (ROOT / 'src/fs.c').read_bytes()}
    includes = ['-I', str(ROOT), '-I', str(ROOT / 'src'), '-I', str(build)]
    entry, guest = directory / 'entry.o', directory / 'guest.o'
    subprocess.run(['nasm', '-f', 'elf', '-Dkmain=fs_crc_guest', '-p', str(build / 'layout.inc'),
                    str(ROOT / 'src/kernel_entry.asm'), '-o', str(entry)], check=True)
    subprocess.run([tool('gcc'), *FLAGS, *includes, '-c', str(ROOT / 'tests/fs_crc_guest.c'),
                    '-o', str(guest)], check=True)
    objects = [str(p) for p in sorted(build.glob('*.o')) if p.name not in ('kernel_entry.o', 'fs.o')]
    info = {'compiler': subprocess.check_output([tool('gcc'), '--version'], text=True).splitlines()[0],
            'qemu': subprocess.check_output(['qemu-system-i386', '--version'], text=True).splitlines()[0],
            'baseline_ref': baseline, 'flags': FLAGS,
            'common_objects_sha256': {Path(p).name: sha(Path(p).read_bytes()) for p in objects}}
    floppies = {}
    for label, source in sources.items():
        source_path, obj = directory / (label + '.c'), directory / (label + '.o')
        source_path.write_bytes(source)
        subprocess.run([tool('gcc'), *FLAGS, *includes, '-c', str(source_path), '-o', str(obj)], check=True)
        elf, binary = directory / (label + '.elf'), directory / (label + '.bin')
        subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '--defsym=fs_clock=crc_benchmark_clock', '-o', str(elf),
                        str(entry), *objects, str(obj), str(guest)], check=True)
        subprocess.run([tool('objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
        disk = bytearray(C['DISK_SECTORS'] * 512)
        disk[:512] = (build / 'boot.bin').read_bytes()
        install_kernel(disk, binary.read_bytes(), C)
        floppies[label] = bytes(disk)
        info[label] = {'source_sha256': sha(source), 'kernel_bytes': binary.stat().st_size,
                       'object_size': subprocess.check_output([tool('size'), str(obj)], text=True).strip()}
    return floppies, info


def verify_saved(raw, original):
    assert raw[:512] == volume.data_marker() and raw[-512:] == bytes(512)
    layout = volume.disk_layout(raw)
    # Slot 1 is the prior committed snapshot and must be completely unchanged.
    untouched = layout.lbas[1] * 512
    assert raw[untouched:] == original[untouched:]
    for slot in (0, 1):
        start = layout.lbas[slot] * 512
        _, version, count, length, checksum, generation, header_crc = struct.unpack_from('<7I', raw, start)
        assert (version, count, length, generation) == (4, 256, 8387584, 19 if slot == 0 else 18)
        assert header_crc == zlib.crc32(raw[start:start + 24])
        assert checksum == zlib.crc32(raw[start + 512:start + 512 + length])
        decoded = volume.decode(raw, slot)
        expected = volume.decode(original, slot)[1]
        if slot == 0:
            expected[1]['modified'] = 1234567890
        assert decoded == (generation, expected)


def run(directory, label, boot, data, timeout):
    floppy, disk = directory / (label + '-boot.img'), directory / (label + '-data.img')
    floppy.write_bytes(boot); disk.write_bytes(data)
    log, errors = directory / (label + '.log'), directory / (label + '.stderr')
    log.write_text('')
    command = ['qemu-system-i386', '-accel', 'tcg', '-m', '64M', '-vga', 'std', '-boot', 'a',
               '-drive', f'file={floppy},format=raw,index=0,if=floppy',
               '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback',
               '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot']
    observed = {}; started = time.monotonic()
    with errors.open('w') as stderr:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=stderr)
        try:
            while time.monotonic() - started < timeout:
                text = log.read_text(); now = time.monotonic()
                for phase in PHASES:
                    for suffix in ('BEGIN', 'TICKS='):
                        token = f'CRC-BENCH-{phase}-{suffix}'
                        if token in text: observed.setdefault(token, now)
                if 'CRC-BENCH-PASS' in text:
                    pass_wall = now - started
                    break
                if 'PANIC:' in text or process.poll() is not None:
                    raise AssertionError(text + errors.read_text())
                time.sleep(.01)
            else:
                raise AssertionError('benchmark timeout: ' + log.read_text())
        finally:
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
                raise AssertionError('QEMU did not close normally')
    total_wall = time.monotonic() - started
    assert floppy.read_bytes() == boot
    saved = disk.read_bytes(); verify_saved(saved, data)
    ticks = {phase.lower(): int(re.search(f'CRC-BENCH-{phase}-TICKS=(\\d+)', text)[1]) for phase in PHASES}
    phase_wall = {phase.lower(): observed[f'CRC-BENCH-{phase}-TICKS='] - observed[f'CRC-BENCH-{phase}-BEGIN']
                  for phase in PHASES}
    result = {'label': label, 'ticks': ticks, 'phase_wall_seconds': phase_wall,
              'boot_to_pass_wall_seconds': pass_wall, 'total_wall_seconds': total_wall, 'saved_sha256': sha(saved)}
    print(json.dumps(result), flush=True)
    return result


def main(args):
    directory = Path(tempfile.mkdtemp(prefix='baseos-crc-benchmark-'))
    print(f'QEMU evidence: {directory}', flush=True)
    succeeded = False
    try:
        floppies, info = build_variants(args.build.resolve(), directory, args.baseline_ref)
        data = full_volume(); results = []
        # Alternate which version runs first to expose drift rather than hide it.
        for pair in range(args.runs):
            for variant in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
                results.append(run(directory, f'{variant}-{pair + 1}', floppies[variant], data, args.timeout))
        assert len({r['saved_sha256'] for r in results}) == 1, 'persisted bytes differ across variants/runs'
        summary = {}
        for variant in ('before', 'after'):
            selected = [r for r in results if r['label'].startswith(variant)]
            summary[variant] = {phase.lower(): statistics.median(r['ticks'][phase.lower()] for r in selected)
                                for phase in PHASES}
            summary[variant]['boot_to_pass_wall_seconds'] = statistics.median(r['boot_to_pass_wall_seconds'] for r in selected)
        report = {'environment': info, 'timer_hz': 70, 'host_poll_seconds': .01,
                  'data_sha256': sha(data), 'runs': results, 'medians': summary}
        (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Median results: ' + json.dumps(summary), flush=True)
        print('PASS: all snapshots independently decoded; all saved bytes identical.', flush=True)
        succeeded = True
    finally:
        if succeeded and not args.keep: shutil.rmtree(directory)
        else: print(f'Retained evidence: {directory}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--baseline-ref', required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    if args.runs < 1: parser.error('--runs must be positive')
    main(args)
