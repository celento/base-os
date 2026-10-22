"""FAT exchange disks for programs: a FAT32 disk made by mkfs.fat and mtools
on the secondary IDE master shows up as /disk1, and examples/elf/fatdisk.c
reads, writes, renames and removes files on it through the C library. The
host then checks the disk with fsck.fat and reads the results with mtools.

python3 tools/fat_volume_test.py build
Needs build/toolchain, dosfstools and mtools.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

from elf_libc_test import TOOLCHAIN, data_disk
from elf_runtime_test import ROOT, boot

ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8')


def pattern(i):
    return (i * 2654435761 & 0xFFFFFFFF) >> 24


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-fat-volume-'))
    print('Work directory:', work, flush=True)
    program = work / 'fatdisk'
    subprocess.run([str(TOOLCHAIN / 'i686-baseos-gcc'), '-O2', '-Wall', '-Wextra',
                    str(ROOT / 'examples/elf/fatdisk.c'), '-o', str(program)], check=True)
    subprocess.run([str(TOOLCHAIN / 'i686-baseos-strip'), str(program)], check=True)
    disk = data_disk(work, {'fatdisk': program.read_bytes()})
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', '-n', 'EXCHANGE', str(fat), str(64 * 1024)],
                   check=True, capture_output=True)
    (work / 'README.TXT').write_bytes(b'hello from the host\n')
    (work / 'OLD SAVE.DAT').write_bytes(b'save' * 100)
    subprocess.run(['mcopy', '-i', str(fat), str(work / 'README.TXT'), '::'], check=True, env=ENV)
    subprocess.run(['mmd', '-i', str(fat), '::GAMES'], check=True, env=ENV)
    subprocess.run(['mcopy', '-i', str(fat), str(work / 'OLD SAVE.DAT'), '::GAMES'], check=True, env=ENV)
    subprocess.run(['mcopy', '-i', str(fat), str(work / 'README.TXT'), '::LOCKED.TXT'], check=True, env=ENV)
    subprocess.run(['mattrib', '-i', str(fat), '+r', '::LOCKED.TXT'], check=True, env=ENV)
    text = boot(build, work, [('/Programs/fatdisk', 'fat', 0, 'fat disk ok')], disk,
                extra=('-drive', f'file={fat},format=raw,if=ide,index=2'))
    assert 'FAT: mounted /disk1' in text, text
    check = subprocess.run(['fsck.fat', '-n', str(fat)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr
    out = work / 'out'
    out.mkdir()
    subprocess.run(['mcopy', '-s', '-n', '-i', str(fat), '::/*', str(out)], check=True, env=ENV)
    big = (out / 'New' / 'A big file.bin').read_bytes()
    size = (3 << 20) - 100
    expected = bytearray(pattern(i) for i in range(size))
    expected[1000001:1000008] = b'patched'
    assert big == bytes(expected), 'big file differs'
    assert (out / 'GAMES' / 'Renamed Note.txt').read_bytes() == b'written on BaseOS\nappended\n'
    assert (out / 'GAMES' / 'OLD SAVE.DAT').read_bytes() == b'save' * 100
    assert (out / 'LOCKED.TXT').read_bytes() == b'hello from the host\n'
    assert sorted(p.name for p in (out / 'New').iterdir()) == ['A big file.bin']
    print('FAT_VOLUME_PASS: /disk1 read and written by a C program, clean under fsck.fat.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
