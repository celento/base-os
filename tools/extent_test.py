"""Boot a deliberately large kernel from the split loader extents."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
from layout import constants
from smoke_test import run
from update_image import install_kernel

build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parents[1]
work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-extent-'))
print(work, flush=True)
def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name
subprocess.run(['nasm', '-f', 'elf', '-Dkmain=extent_guest', '-p', str(build / 'layout.inc'),
                str(root / 'src/kernel_entry.asm'), '-o', str(work / 'entry.o')], check=True)
c = constants()
sections = subprocess.check_output([tool('size'), '-A', str(build / 'kernel.elf')], text=True)
text_size = next(int(line.split()[1]) for line in sections.splitlines() if line.startswith('.text '))
padding = max(16, c['KERNEL_PRIMARY_SECTORS'] * 512 - text_size + 512)
subprocess.run(['nasm', '-f', 'elf', f'-DEXTENT_PADDING={padding}', str(root / 'tests/extent_guest.asm'),
                '-o', str(work / 'extent.o')], check=True)
objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel_entry.o']
subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                '-z', 'noexecstack', '-o', str(work / 'kernel.elf'),
                str(work / 'entry.o'), *objects, str(work / 'extent.o')], check=True)
subprocess.run([tool('objcopy'), '-O', 'binary', str(work / 'kernel.elf'), str(work / 'kernel.bin')], check=True)
symbols = subprocess.check_output([tool('nm'), '-n', str(work / 'kernel.elf')], text=True)
probe = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' beyond_primary'))
assert probe >= c['KERNEL_LOAD_ADDR'] + c['KERNEL_PRIMARY_SECTORS'] * 512
kernel = (work / 'kernel.bin').read_bytes()
assert len(kernel) > c['KERNEL_PRIMARY_SECTORS'] * 512
image = bytearray(c['DISK_SECTORS'] * 512)
image[:512] = (build / 'boot.bin').read_bytes()
install_kernel(image, kernel, c)
path = work / 'disk.img'
path.write_bytes(image)
log = run(path, work, 'split-kernel', 'DESKTOP', seconds=30)
assert 'KERNEL-EXTENT-PASS' in log
print(f'Split kernel boot passed ({len(kernel)} bytes).', flush=True)
