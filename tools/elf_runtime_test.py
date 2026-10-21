"""ELF runtime fixture: loader, address spaces, threads and system calls.

python3 tools/elf_runtime_test.py build
Builds tests/elf_program.c with the host compiler as a static ELF32 program,
links tests/elf_guest.c in place of the desktop shell against the production
kernel objects, and boots it with 512 MiB so one check can touch 128 MiB.
Serial output only. Logs and images stay in the printed temporary directory.
"""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from layout import constants
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]


def tool(name):
    cross = 'x86_64-elf-' + name
    return cross if shutil.which(cross) else name


PROGRAM_STEPS = (
    ('hello', 7, 'argc 3\\nlast-argument\\n'),
    ('memory', 0, 'ELF memory 128 MiB ok'),
    ('threads', 0, 'ELF threads ok'),
    ('clock', 0, 'ELF clock ok'),
    ('scheduler', 0, 'ELF scheduler ok'),
    ('fault', 139, 'ELF fault next'),
    ('gate', 159, None),
    ('hello', 7, None),
    ('empty-segment', -22, None),
)


def freestanding_program(work):
    program = work / 'elf_program'
    subprocess.run([tool('gcc'), '-m32', '-static', '-nostdlib', '-ffreestanding', '-fno-pie',
                    '-no-pie', '-fno-stack-protector', '-Os', '-Wall', '-Wextra', '-I', str(ROOT),
                    '-Wl,-Ttext-segment=0x40100000', '-Wl,-z,max-page-size=4096',
                    '-Wl,--build-id=none', '-o', str(program), str(ROOT / 'tests/elf_program.c')],
                   check=True)
    return program


def empty_segment_program(program, work):
    """The program with its GNU_STACK header turned into a PT_LOAD whose
    memory size is zero but whose file bytes run past the end of the image.
    The loader must refuse it rather than copy from beyond the file."""
    data = bytearray(program.read_bytes())
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    headers = [struct.unpack_from('<8I', data, phoff + i * phentsize) for i in range(phnum)]
    stack = next(i for i, h in enumerate(headers) if h[0] == 0x6474E551)
    first = next(h for h in headers if h[0] == 1)
    struct.pack_into('<8I', data, phoff + stack * phentsize,
                     1, len(data) - 16, first[2], first[3], 0x10000, 0, 4, 0x1000)
    crafted = work / 'elf_empty_segment'
    crafted.write_bytes(bytes(data))
    return crafted


def c_string(value):
    return '0' if value is None else '"' + value + '"'


def write_steps(work, steps):
    """steps: (program, argv[1], exit status, expected output or None). A
    program is a host path to embed, or a str path on the guest's volume."""
    arrays, names = [], {}
    for path, *_ in steps:
        if path not in names and not isinstance(path, str):
            names[path] = f'program_{len(names)}'
            arrays.append(subprocess.run([sys.executable, str(ROOT / 'tools/bin2c.py'), str(path),
                                          names[path]], check=True, capture_output=True,
                                         text=True).stdout)
    def row(path, check, status, output):
        image = f'0, 0, "{path}"' if isinstance(path, str) else f'{names[path]}, sizeof {names[path]}, 0'
        return f'    {{{image}, "{check}", {status}, {c_string(output)}}}'
    rows = ',\n'.join(row(*step) for step in steps)
    (work / 'elf_steps.h').write_text(''.join(arrays) + f'static const ElfStep steps[] = {{\n{rows}\n}};\n')


def build_fixture(build, work, steps=None):
    if steps is None:
        program = freestanding_program(work)
        crafted = empty_segment_program(program, work)
        steps = [(crafted if row[0] == 'empty-segment' else program, *row) for row in PROGRAM_STEPS]
    write_steps(work, steps)
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-g', '-Wall', '-Wextra', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin', '-mno-sse',
                    '-mno-mmx', '-msoft-float', '-I', str(ROOT), '-I', str(ROOT / 'src'),
                    '-I', str(build), '-I', str(work), '-c', str(ROOT / 'tests/elf_guest.c'),
                    '-o', str(work / 'guest.o')], check=True)
    objects = [str(p) for p in sorted(build.glob('*.o')) if p.name != 'desktop.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(work / 'kernel.elf'), str(work / 'guest.o'), *objects],
                   check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(work / 'kernel.elf'), str(work / 'kernel.bin')],
                   check=True)
    c = constants()
    image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (work / 'kernel.bin').read_bytes(), c)
    (work / 'boot.img').write_bytes(image)


def boot(build, work, steps=None, disk=None):
    """Boot the fixture; returns the serial log once every step passed. disk
    is an optional IDE data volume holding programs named by path."""
    build = pathlib.Path(build).resolve()
    build_fixture(build, work, steps)
    log = work / 'serial.log'
    qemu = subprocess.Popen(['qemu-system-i386', '-m', '512M', '-vga', 'std', '-drive',
                             f'file={work / "boot.img"},format=raw,index=0,if=floppy',
                             '-display', 'none', '-serial', f'file:{log}', '-no-reboot',
                             *(('-drive', f'file={disk},format=raw,index=0,if=ide') if disk else ())],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            text = log.read_text(errors='replace') if log.exists() else ''
            if 'ELF-RUNTIME-PASS' in text:
                break
            if 'ELF-RUNTIME-FAIL' in text or 'PANIC' in text or qemu.poll() is not None:
                raise AssertionError(text)
            time.sleep(.2)
        else:
            raise AssertionError('timeout:\n' + log.read_text(errors='replace'))
    finally:
        qemu.kill()
        qemu.wait()
    return log.read_text(errors='replace')


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-runtime-'))
    print('Work directory:', work, flush=True)
    text = boot(build, work)
    for needle in ('ELF hello from ring 3', 'ELF memory 128 MiB ok', 'ELF threads ok', 'ELF scheduler ok',
                   'uproc: page fault at 0x40000000'):
        assert needle in text, needle
    print('ELF_RUNTIME_PASS: loader, 128 MiB demand paging, threads, futexes, fault isolation.',
          flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
