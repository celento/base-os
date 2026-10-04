"""Normal boots validating packed reconstruction, stack, GDT and larger raw code.

python3 tools/kernel_space_test.py build
Uses fresh disposable images, including an ordinary valid legacy upgrade. Never
reads saved build/*.img files; does not inject faults or malformed guest input.
All logs and images are retained in the printed temporary directory.
"""
import argparse
import json
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

from layout import constants
from qemu_session import DesktopSession
from update_image import install_kernel, install_packed_kernel, update
from kernel_pack import LZ4, RAW, pack_kernel, unpack_kernel
import volume

ROOT = pathlib.Path(__file__).resolve().parents[1]
C = constants()


def tool(name):
    cross = 'x86_64-elf-' + name
    return cross if shutil.which(cross) else name


def symbols(elf):
    result = {}
    for line in subprocess.check_output([tool('nm'), '-n', str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result


def install(path, build, kernel):
    disk = bytearray(C['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(disk, kernel, C)
    path.write_bytes(disk)



def check_runtime(session, names, kernel):
    session.command('stop')
    try:
        registers = session.command('human-monitor-command', {'command-line': 'info registers'})
        eip = int(re.search(r'\bEIP=([0-9a-fA-F]+)', registers)[1], 16)
        esp = int(re.search(r'\bESP=([0-9a-fA-F]+)', registers)[1], 16)
        gdt = int(re.search(r'\bGDT=\s*([0-9a-fA-F]+)', registers)[1], 16)
        assert C['KERNEL_LOAD_ADDR'] <= eip < names['__load_end'], registers
        assert C['STACK_BOTTOM'] <= esp < C['STACK_TOP'], registers
        assert gdt == names['gdt_start'], registers
        assert names['__kernel_end'] <= C['STACK_BOTTOM']
        # C legitimately changes the GDT and .data. Compare every immutable
        # instruction/rodata byte, excluding just the embedded GDT, not samples.
        immutable_bytes = 0
        for start, end in ((C['KERNEL_LOAD_ADDR'], names['gdt_start']),
                           (names['__bootstrap_end'], names['__text_end']),
                           (names['__rodata_start'], names['__rodata_end'])):
            offset = start - C['KERNEL_LOAD_ADDR']
            expected = kernel[offset:offset + end - start]
            assert session.memory(start, len(expected)) == expected, hex(start)
            immutable_bytes += len(expected)
        # Only the plain prefix is raw at the BIOS stage. Its initial GDT has
        # CPU-updated Accessed bits, so compare immutable ranges around it.
        for start, end in ((0, names['gdt_start'] - C['KERNEL_LOAD_ADDR']),
                           (names['__bootstrap_end'] - C['KERNEL_LOAD_ADDR'],
                            C['KERNEL_BOOTSTRAP_BYTES'])):
            assert session.memory(C['KERNEL_STAGE_ADDR'] + start, end - start) == kernel[start:end]
        gdtr = session.memory(C['BOOTINFO_ADDR'] + 0x40, 6)
        assert struct.unpack('<HI', gdtr) == (47, names['gdt_start'])
        # The active TSS uses the existing independent 64 KiB task syscall stack.
        descriptor = session.memory(names['gdt_tss'], 8)
        tss = descriptor[2] | descriptor[3] << 8 | descriptor[4] << 16 | descriptor[7] << 24
        assert descriptor[5] & 15 == 11, descriptor  # Busy 32-bit TSS after LTR.
        esp0 = struct.unpack('<I', session.memory(tss + 4, 4))[0]
        assert esp0 == C['TASK_INTERRUPT_STACK_BASE'] + C['TASK_INTERRUPT_STACK_CAPACITY']
        info = session.memory(C['BOOTINFO_ADDR'], 26)
        assert struct.unpack_from('<I', info)[0] == C['BOOTINFO_MAGIC']
        assert 0 < struct.unpack_from('<H', info, 16)[0] <= C['E820_MAX']
        assert info[18:20] == bytes((0, 36))
        if 'kernel_tail_marker' in names:
            tail = names['kernel_tail_marker']
            offset = tail - C['KERNEL_LOAD_ADDR']
            assert offset == len(kernel) - 32
            assert session.memory(tail, 32) == kernel[-32:]
            start, end = names['kernel_growth_bss'], names['kernel_growth_bss_end']
            assert session.memory(start, end - start) == bytes(end - start)
        return dict(eip=hex(eip), esp=hex(esp), gdt=hex(gdt), task_esp0=hex(esp0),
                    kernel_bytes=len(kernel), packed_bytes=len(pack_kernel(kernel, C)),
                    immutable_bytes_verified=immutable_bytes,
                    kernel_end=hex(names['__kernel_end']),
                    memory_margin=C['STACK_BOTTOM'] - names['__kernel_end'])
    finally:
        session.command('cont')


def boot(build, image, directory, label, names, kernel, loaded=False, codec=LZ4):
    with DesktopSession(build, 'kernel-space-' + label, image=image) as session:
        session.boot()
        log = session.log.read_text()
        if loaded:
            assert 'FS loaded from disk' in log and 'FS seeded fresh' not in log, log
        result = check_runtime(session, names, kernel)
        result.update(codec=codec, packed_bytes=len(pack_kernel(kernel, C, codec=codec)))
        # Ordinary first-boot autosaves finish before this owned QEMU is stopped.
        time.sleep(4)
        shutil.copyfile(session.log, directory / (label + '.log'))
        if label == 'fresh':
            shutil.copyfile(session.screenshot('desktop.png'), directory / 'desktop.png')
    assert volume.load(image.read_bytes())
    print(label + ': ' + json.dumps(result), flush=True)
    return result


def legacy_image(path):
    """A valid v3 filesystem in an old-size image, using its original first LBA."""
    disk = bytearray(2880 * 512)
    content = b'Legacy relocation check\n' + bytes(range(256))
    payload = bytearray()
    for ident, name, data in ((0, '', b''), (1, 'old.bin', content)):
        payload += struct.pack('<HhBBHI24sI', ident, 0 if ident else -1,
                               int(not ident), 0, 0, len(data), name.encode(), 0)
        payload += data
    header = struct.pack('<6I', volume.MAGIC, 3, 2, len(payload), zlib.crc32(payload), 7)
    header += struct.pack('<I', zlib.crc32(header))
    start = C['FS_DISK_LBA'] * 512
    disk[start:start + 512] = header.ljust(512, b'\0')
    disk[start + 512:start + 512 + len(payload)] = payload
    assert volume.load(disk)[1] == 7
    path.write_bytes(disk)
    return bytes(disk), content


def full_kernel(build, directory):
    obj, elf, binary = [directory / ('full.' + suffix) for suffix in ('o', 'elf', 'bin')]
    def link(padding):
        subprocess.run(['nasm', '-f', 'elf', f'-DKERNEL_TAIL_PADDING={padding}',
                        str(ROOT / 'tests/kernel_space_guest.asm'), '-o', str(obj)], check=True)
        objects = [build / 'kernel_entry.o'] + [p for p in sorted(build.glob('*.o'))
                                               if p.name != 'kernel_entry.o']
        subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '-o', str(elf), *map(str, objects), str(obj)], check=True)
        subprocess.run([tool('objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
    link(0)
    target = 0xA0000  # 640 KiB initialized, beyond the old raw disk ceiling.
    padding = target - binary.stat().st_size
    assert padding >= 0, 'normal kernel exceeds the 640 KiB growth fixture'
    link(padding)
    data = binary.read_bytes()
    assert len(data) == target
    assert len(data) > C['KERNEL_SECTORS'] * 512
    packed = pack_kernel(data, C)
    assert len(packed) > C['KERNEL_PRIMARY_SECTORS'] * 512
    assert unpack_kernel(packed, C) == data
    names = symbols(elf)
    assert names['__kernel_end'] - C['KERNEL_LOAD_ADDR'] > 0x80000
    return data, names


def main(build, quick=False):
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-kernel-space-'))
    print('Evidence: ' + str(directory), flush=True)
    kernel = (build / 'kernel.bin').read_bytes()
    names = symbols(build / 'kernel.elf')
    image = directory / 'fresh.img'
    install(image, build, kernel)
    results = {'fresh': boot(build, image, directory, 'fresh', names, kernel)}
    results['reboot'] = boot(build, image, directory, 'reboot', names, kernel, loaded=True)
    if not quick:
        raw_bytes = len(kernel) + C['KERNEL_BOOTSTRAP_BYTES'] + C['KERNEL_PACK_HEADER_BYTES']
        if raw_bytes <= C['KERNEL_SECTORS'] * C['SECTOR_SIZE']:
            raw_path = directory / 'raw-codec.img'
            raw_disk = bytearray(C['DISK_SECTORS'] * 512)
            raw_disk[:512] = (build / 'boot.bin').read_bytes()
            install_packed_kernel(raw_disk, pack_kernel(kernel, C, codec=RAW), C)
            raw_path.write_bytes(raw_disk)
            results['raw-codec'] = boot(build, raw_path, directory, 'raw-codec', names, kernel, codec=RAW)
        else:
            results['raw-codec'] = {'skipped': 'Production image exceeds the optional uncompressed disk envelope'}
            print('raw-codec: skipped; production image requires compression', flush=True)
        old_path = directory / 'legacy.img'
        old, content = legacy_image(old_path)
        update(old_path, build / 'boot.bin', build / 'kernel.bin')
        upgraded = old_path.read_bytes()
        start = C['FS_DISK_LBA'] * 512
        assert upgraded[start:len(old)] == old[start:]
        assert next(directory.glob('legacy.img.*.bak')).read_bytes() == old
        results['legacy-upgrade'] = boot(build, old_path, directory, 'legacy-upgrade', names, kernel, loaded=True)
        nodes = volume.load(old_path.read_bytes())[2]
        assert nodes[volume.resolve(nodes, '/old.bin')]['data'] == content
        large, large_names = full_kernel(build, directory)
        full = directory / 'full.img'
        install(full, build, large)
        packed = pack_kernel(large, C)
        on_disk = full.read_bytes()
        first = C['KERNEL_PRIMARY_SECTORS'] * 512
        code = on_disk[512:512 + first] + on_disk[C['KERNEL_EXT_LBA'] * 512:]
        assert code[:len(packed)] == packed
        assert unpack_kernel(code[:len(packed)], C) == large
        results['expanded-initialized'] = boot(build, full, directory, 'expanded-initialized', large_names, large)
        results['expanded-reboot'] = boot(build, full, directory, 'expanded-reboot', large_names, large, loaded=True)
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Relocated kernel checks passed. Evidence: ' + str(directory), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--quick', action='store_true', help='Only normal fresh and reboot checks')
    args = parser.parse_args()
    main(args.build.resolve(), args.quick)
