"""QEMU foundation checks on disposable copies; never boots the user's image.
Usage: python3 tools/smoke_test.py /path/to/build
"""
import argparse
import fcntl
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from layout import constants

C = constants()


def run(image, directory, label, expected, memory='32M', vga='std', seconds=15, ticks_address=None):
    log = directory / (label + '.log')
    log.write_text('')
    errors = directory / (label + '.stderr')
    with errors.open('w') as output:
        p = subprocess.Popen(['qemu-system-i386', '-m', memory, '-vga', vga,
                              '-drive', f'file={image},format=raw,index=0,if=floppy',
                              '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none',
                              '-qmp', 'stdio',
                              '-no-reboot'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=output)
        try:
            deadline = time.monotonic() + seconds
            seen = False
            while time.monotonic() < deadline:
                text = log.read_text()
                if expected in text:
                    seen = True
                    # Allow desktop autosave to finish before stopping.
                    if expected == 'DESKTOP':
                        time.sleep(4)
                        if ticks_address is not None:
                            # Image replacement must be refused while QEMU owns it.
                            with image.open('r+b') as opened:
                                try:
                                    fcntl.lockf(opened, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                except BlockingIOError:
                                    pass
                                else:
                                    raise AssertionError('QEMU image lock was not detected')
                            json.loads(p.stdout.readline())
                            def command(name, arguments=None):
                                p.stdin.write(json.dumps({'execute': name, 'arguments': arguments or {}}).encode() + b'\n')
                                p.stdin.flush()
                                while True:
                                    result = json.loads(p.stdout.readline())
                                    if 'error' in result: raise AssertionError(result)
                                    if 'return' in result: return result['return']
                            command('qmp_capabilities')
                            def ticks():
                                line = command('human-monitor-command', {'command-line': f'xp /1uw 0x{ticks_address:x}'})
                                return int(line.split(':')[-1].strip())
                            before = ticks(); time.sleep(2); after = ticks()
                            assert 125 <= after - before <= 155, (before, after)
                            command('screendump', {'filename': str(directory / 'desktop.png'), 'format': 'png'})
                            print(f'timer: {after - before} hardware ticks in 2 seconds', flush=True)
                    break
                if p.poll() is not None: break
                time.sleep(.1)
            if not seen:
                raise AssertionError(f'{label}: missing {expected!r}:\n{log.read_text()}\n{errors.read_text()}')
        finally:
            if p.poll() is None: p.terminate()
            p.wait(timeout=5)
    print(f'{label}: {expected}', flush=True)
    return log.read_text()


def valid_slots(image):
    data = image.read_bytes()
    result = []
    for lba in (C['FS_DISK_LBA'], C['FS_SECOND_LBA']):
        start = lba * 512
        magic, version, count, length, crc, generation, header_crc = struct.unpack_from('<7I', data, start)
        if magic == 0x46534f42 and version in (2, 3):
            assert header_crc == zlib.crc32(data[start:start + 24])
            assert crc == zlib.crc32(data[start + 512:start + 512 + length])
            assert count and length
            result.append(generation)
    return result


def main(build, keep):
    nm = 'x86_64-elf-nm' if shutil.which('x86_64-elf-nm') else 'nm'
    symbols = {}
    for line in subprocess.check_output([nm, '-n', str(build / 'kernel.elf')], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3: symbols[parts[2]] = int(parts[0], 16)
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-smoke-'))
    print(f'QEMU logs and test disks: {directory}', flush=True)
    try:
        # Start fresh regardless of the source build image's contents.
        disk = bytearray(C['DISK_SECTORS'] * 512)
        disk[:512] = (build / 'boot.bin').read_bytes()
        kernel = (build / 'kernel.bin').read_bytes()
        disk[512:512 + len(kernel)] = kernel
        image = directory / 'roundtrip.img'; image.write_bytes(disk)
        run(image, directory, 'fresh', 'DESKTOP', ticks_address=symbols['ticks'])
        assert valid_slots(image)
        text = run(image, directory, 'reboot', 'DESKTOP')
        assert 'FS loaded from disk' in text and 'FS seeded fresh' not in text
        assert valid_slots(image)

        for label, code, vector in (
            ('invalid-opcode', b'\x0f\x0b', '00000006'),
            ('divide-error', b'\x31\xd2\x31\xc0\xf7\xf2', '00000000'),
            ('general-protection', b'\x66\xb8\xf8\xff\x8e\xd8', '0000000D'),
        ):
            patched = bytearray(disk)
            offset = 512 + symbols['kmain'] - C['KERNEL_LOAD_ADDR']
            patched[offset:offset + len(code)] = code
            target = directory / f'{label}.img'; target.write_bytes(patched)
            text = run(target, directory, label, 'PANIC: CPU exception')
            assert f'EXCEPTION vector={vector}' in text
            assert f'eip={symbols["kmain"] + (4 if label != "invalid-opcode" else 0):08X}' in text
        target = directory / 'low-memory.img'; target.write_bytes(disk)
        run(target, directory, 'low-memory', 'PANIC: required RAM', memory='8M')
        target = directory / 'no-video.img'; target.write_bytes(disk)
        run(target, directory, 'no-video', 'PANIC: no supported', vga='none')

        # Reassemble only entry code to poison BSS before runtime clearing.
        subprocess.run(['nasm', '-f', 'elf', '-DTEST_DIRTY_BSS', '-p', str(build / 'layout.inc'),
                        str(pathlib.Path(__file__).resolve().parents[1] / 'src/kernel_entry.asm'),
                        '-o', str(directory / 'kernel_entry.o')], check=True)
        ld = 'x86_64-elf-ld' if shutil.which('x86_64-elf-ld') else 'ld'
        objcopy = 'x86_64-elf-objcopy' if shutil.which('x86_64-elf-objcopy') else 'objcopy'
        objects = [str(directory / 'kernel_entry.o')] + [str(p) for p in sorted(build.glob('*.o')) if p.name != 'kernel_entry.o']
        subprocess.run([ld, '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '-o', str(directory / 'dirty.elf'), *objects], check=True)
        subprocess.run([objcopy, '-O', 'binary', str(directory / 'dirty.elf'), str(directory / 'dirty.bin')], check=True)
        poisoned = bytearray(disk)
        data = (directory / 'dirty.bin').read_bytes()
        assert len(data) <= C['KERNEL_SECTORS'] * 512
        poisoned[512:512 + C['KERNEL_SECTORS'] * 512] = bytes(C['KERNEL_SECTORS'] * 512)
        poisoned[512:512 + len(data)] = data
        target = directory / 'dirty-bss.img'; target.write_bytes(poisoned)
        run(target, directory, 'dirty-bss', 'DESKTOP')
        # Exercise multi-track DMA writes and both snapshot locations in QEMU.
        cc = 'x86_64-elf-gcc' if shutil.which('x86_64-elf-gcc') else 'gcc'
        root = pathlib.Path(__file__).resolve().parents[1]
        subprocess.run([cc, '-std=gnu11', '-O2', '-ffreestanding', '-m32', '-fno-pie',
                        '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx',
                        '-msoft-float', '-I', str(root / 'src'), '-c', str(root / 'tests/storage_guest.c'),
                        '-o', str(directory / 'storage_guest.o')], check=True)
        subprocess.run(['nasm', '-f', 'elf', '-Dkmain=storage_guest', '-p', str(build / 'layout.inc'),
                        str(root / 'src/kernel_entry.asm'), '-o', str(directory / 'storage_entry.o')], check=True)
        objects[0] = str(directory / 'storage_entry.o')
        subprocess.run([ld, '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                        '-z', 'noexecstack', '-o', str(directory / 'storage.elf'), *objects,
                        str(directory / 'storage_guest.o')], check=True)
        subprocess.run([objcopy, '-O', 'binary', str(directory / 'storage.elf'), str(directory / 'storage.bin')], check=True)
        storage_disk = bytearray(disk)
        data = (directory / 'storage.bin').read_bytes()
        assert len(data) <= C['KERNEL_SECTORS'] * 512
        storage_disk[512:512 + len(data)] = data
        target = directory / 'storage.img'; target.write_bytes(storage_disk)
        run(target, directory, 'multi-track-storage', 'HARDWARE-STORAGE-PASS', seconds=40)
        assert valid_slots(target) == [1, 2]
        print('All QEMU foundation checks passed.', flush=True)
    finally:
        if not keep: shutil.rmtree(directory)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    main(args.build.resolve(), args.keep)
