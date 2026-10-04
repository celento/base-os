"""Bounded native desktop functional fixture, with serial-file observation only.

Uses disposable floppy/IDE images, no debugger, QMP, monitor or guest-memory access.
The fixture uses the real dispatch functions and native scheduler; it is not a
PS/2 input or pixel test of the production desktop.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from init_data import initialize
from layout import constants
from update_image import install_kernel
from volume import load, resolve

ROOT = Path(__file__).resolve().parents[1]


def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name


def run(build):
    build = build.resolve()
    directory = Path(tempfile.mkdtemp(prefix='baseos-native-launch-'))
    print(directory, flush=True)
    subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie',
                    '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float',
                    '-I', str(ROOT), '-I', str(ROOT / 'src'), '-I', str(build),
                    '-c', str(ROOT / 'tests/native_launch_guest.c'), '-o', str(directory / 'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'),
                    str(directory / 'kernel.bin')], check=True)
    layout = constants()
    image = bytearray(layout['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (directory / 'kernel.bin').read_bytes(), layout)
    floppy = directory / 'floppy.img'; floppy.write_bytes(image)
    data = directory / 'data.img'; initialize(data)
    for label, expected in [('first', 'NATIVE-LAUNCH-FUNCTIONAL-PASS'),
                            ('reboot', 'NATIVE-LAUNCH-REBOOT-PASS')]:
        log = directory / (label + '.log')
        with (directory / (label + '.stderr')).open('w') as errors:
            process = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std',
                                        '-drive', f'file={floppy},format=raw,index=0,if=floppy',
                                        '-drive', f'file={data},format=raw,index=0,if=ide',
                                        '-serial', f'file:{log}', '-display', 'none',
                                        '-monitor', 'none', '-no-reboot'],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + 65
                while time.monotonic() < deadline:
                    text = log.read_text() if log.exists() else ''
                    if expected in text:
                        print(expected, flush=True)
                        break
                    if 'PANIC:' in text or process.poll() is not None:
                        raise AssertionError(text)
                    time.sleep(.1)
                else:
                    raise AssertionError('Timed out: ' + text)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
    _, _, nodes = load(data.read_bytes())
    one = int(nodes[resolve(nodes, '/Documents/counter-2.txt')]['data'])
    two = int(nodes[resolve(nodes, '/Documents/counter-3.txt')]['data'])
    note = nodes[resolve(nodes, '/Documents/sdk-note.txt')]['data']
    assert one >= 3 and two >= 23 and two >= one + 19, (one, two)
    assert note == b'This note was saved by a protected C application.\n'
    print(f'Native dispatch/lifecycle and reboot persistence passed: counters {one}/{two}, exact Notebook bytes.', flush=True)


if __name__ == '__main__':
    run(Path(sys.argv[1] if len(sys.argv) > 1 else 'build'))
