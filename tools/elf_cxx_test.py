"""Modern C++ on the ELF runtime: what DOSBox Staging needs from libstdc++.

python3 tools/elf_cxx_test.py build
Needs build/toolchain (python3 toolchain/build.py), mkfs.fat and mtools. Builds
examples/elf/modern.cpp with -std=c++20 and runs it in the
tools/elf_runtime_test.py fixture: std::jthread, condition variables, futures,
timed sleeps, std::filesystem on the native volume, iostreams, std::format,
exceptions and <random>.
"""
import pathlib
import subprocess
import sys
import tempfile

from elf_libc_test import TOOLCHAIN
from elf_runtime_test import ROOT, boot
from fat_volume_test import ENV


def run(build):
    gxx = TOOLCHAIN / 'i686-baseos-g++'
    if not gxx.exists():
        raise SystemExit('Build the toolchain first: python3 toolchain/build.py')
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-cxx-'))
    print('Work directory:', work, flush=True)
    program = work / 'modern'
    subprocess.run([str(gxx), '-std=c++20', '-O2', '-Wall', '-Wextra', str(ROOT / 'examples/elf/modern.cpp'),
                    '-o', str(program)], check=True)
    subprocess.run([str(TOOLCHAIN / 'i686-baseos-strip'), str(program)], check=True)
    # Programs this size go on a FAT disk, which has no whole-file limit.
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', str(fat), str(64 * 1024)], check=True, capture_output=True)
    subprocess.run(['mcopy', '-i', str(fat), str(program), '::MODERN'], check=True, env=ENV)
    text = boot(build, work, [('/disk1/MODERN', 'modern', 0, 'modern c++ ok: 4 threads, sum 5050')],
                extra=('-boot', 'a', '-drive', f'file={fat},format=raw,if=ide,index=2'))
    assert 'modern: ' not in text.replace('modern c++ ok', ''), text
    print('ELF_CXX_PASS: C++20 threads, filesystem, iostreams and format run on the ELF runtime.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
