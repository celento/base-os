"""C and C++ programs built with the cross toolchain, run on the ELF runtime.

python3 tools/elf_libc_test.py build
Needs build/toolchain (python3 toolchain/build.py). Builds examples/elf/hello.c
and hello.cpp, then boots them in the tools/elf_runtime_test.py fixture:
stdio, malloc of 32 MiB, POSIX threads with a mutex, gmtime, C++ containers,
static constructors and exceptions.
"""
import pathlib
import subprocess
import sys
import tempfile

from elf_runtime_test import ROOT, boot
from init_data import initialize
from volume import DATA_LAYOUT, encode_snapshot, load

TOOLCHAIN = ROOT / 'build/toolchain/bin'


def data_disk(work, files):
    """A fresh data volume with files: {name: bytes} under /Programs."""
    disk = work / 'data.img'
    initialize(disk)
    data = bytearray(disk.read_bytes())
    nodes = {0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
             1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0)}
    for name, content in files.items():
        nodes[len(nodes)] = dict(parent=1, name=name, directory=0, app=0, data=content, modified=0)
    header, payload = encode_snapshot(nodes, DATA_LAYOUT, 1)
    start = DATA_LAYOUT.lbas[0] * 512
    data[start:start + 512] = header.ljust(512, b'\0')
    data[start + 512:start + 512 + len(payload)] = payload
    assert len(load(data)[2]) == len(nodes)
    disk.write_bytes(data)
    return disk


def run(build):
    gcc, gxx = TOOLCHAIN / 'i686-baseos-gcc', TOOLCHAIN / 'i686-baseos-g++'
    if not gxx.exists():
        raise SystemExit('Build the toolchain first: python3 toolchain/build.py')
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-libc-'))
    print('Work directory:', work, flush=True)
    hello_c, hello_cpp = work / 'hello', work / 'hello-cpp'
    subprocess.run([str(gcc), '-O2', '-Wall', '-Wextra', str(ROOT / 'examples/elf/hello.c'),
                    '-o', str(hello_c), '-lpthread'], check=True)
    subprocess.run([str(gxx), '-O2', '-Wall', '-Wextra', str(ROOT / 'examples/elf/hello.cpp'),
                    '-o', str(hello_cpp)], check=True)
    # The fixture embeds the programs in the kernel image, so drop symbols.
    subprocess.run([str(TOOLCHAIN / 'i686-baseos-strip'), str(hello_c), str(hello_cpp)], check=True)
    print('Stripped sizes:', hello_c.stat().st_size, hello_cpp.stat().st_size, flush=True)
    disk = data_disk(work, {'hello': hello_c.read_bytes(), 'hello-cpp': hello_cpp.read_bytes()})
    text = boot(build, work, [
        ('/Programs/hello', 'c', 0, 'threads total 1000000 (expected 1000000)'),
        ('/Programs/hello-cpp', 'cpp', 0, 'exceptions work'),
    ], disk)
    for needle in ('Hello from C on BaseOS (3 arguments)', 'malloc and memset of 32 MiB ok',
                   'Hello from C++ on BaseOS: apps first, 4 words, 2 counters'):
        assert needle in text, needle
    print('ELF_LIBC_PASS: mlibc stdio, heap, pthreads and time; libstdc++ containers and exceptions.',
          flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
