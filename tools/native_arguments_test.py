"""Bounded native argument ABI and Terminal tests in two disposable QEMU boots."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from build_app import build as build_app, tool
from data_volume_test import run
from layout import constants
from native_sdk_test import snapshot
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]


def prepare(build, directory):
    app = directory / 'arguments.bex'
    build_app(ROOT / 'tests/native_argument_app.c', app)
    files = [(1, 'arguments.bex', app.read_bytes()), (2, 'a sample.txt', b'One ordinary document.\n')]
    (directory / 'data.img').write_bytes(snapshot(files))
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(ROOT),
                    '-I', str(ROOT / 'src'), '-I', str(build), '-c',
                    str(ROOT / 'tests/native_arguments_guest.c'), '-o', str(directory / 'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'), str(directory / 'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'), str(directory / 'kernel.bin')], check=True)
    c = constants(); image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (directory / 'kernel.bin').read_bytes(), c)
    (directory / 'boot.img').write_bytes(image)


def main(build, compile_only=False):
    directory = Path(tempfile.mkdtemp(prefix='baseos-native-arguments-'))
    print(directory, flush=True); prepare(build, directory)
    if compile_only:
        print('Compiled guest and disposable images; QEMU not started.'); return
    run(directory / 'boot.img', directory / 'data.img', directory, 'arguments', 'NATIVE-ARGUMENTS-PASS')
    run(directory / 'boot.img', directory / 'data.img', directory, 'arguments-reboot', 'NATIVE-ARGUMENTS-REBOOT-PASS')
    nodes = volume.load((directory / 'data.img').read_bytes())[2]
    for owner, expected in ((1, b'/Documents/a sample.txt'), (2, b'/Documents/a sample.txt'), (3, b'none')):
        assert nodes[volume.resolve(nodes, f'/Documents/argument-{owner}.txt')]['data'] == expected
    print('Native argument copy, 128-byte boundary, per-owner isolation, legacy exec/start, quoted/relative paths and reboot passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path); parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args(); main(args.build.resolve(), args.compile_only)
