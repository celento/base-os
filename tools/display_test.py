"""Verify real resolution changes, confirmation, timeout, and reboot persistence."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
from layout import constants
from update_image import install_kernel
from smoke_test import run

build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parents[1]
work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-display-'))
print(work, flush=True)
def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name
subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie', '-fno-stack-protector',
                '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(root),
                '-I', str(root/'src'), '-I', str(build), '-c', str(root/'tests/display_guest.c'),
                '-o', str(work/'kernel.o')], check=True)
objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
subprocess.run([tool('ld'), '-T', str(build/'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                '-z', 'noexecstack', '-o', str(work/'kernel.elf'), str(work/'kernel.o'), *objects], check=True)
subprocess.run([tool('objcopy'), '-O', 'binary', str(work/'kernel.elf'), str(work/'kernel.bin')], check=True)
c = constants(); disk = bytearray(c['DISK_SECTORS'] * 512)
disk[:512] = (build/'boot.bin').read_bytes(); install_kernel(disk, (work/'kernel.bin').read_bytes(), c)
image = work/'disk.img'; image.write_bytes(disk)
log = work/'display.log'; log.write_text('')
process = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std', '-drive',
                           f'file={image},format=raw,index=0,if=floppy', '-serial', f'file:{log}',
                           '-display', 'none', '-qmp', 'stdio', '-no-reboot'],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def qmp(name, args=None):
    process.stdin.write(json.dumps({'execute':name,'arguments':args or {}}).encode()+b'\n'); process.stdin.flush()
    while True:
        response = json.loads(process.stdout.readline())
        if 'error' in response: raise AssertionError(response)
        if 'return' in response: return response['return']
try:
    json.loads(process.stdout.readline()); qmp('qmp_capabilities')
    seen = set(); deadline = time.monotonic()+50
    while time.monotonic() < deadline:
        text = log.read_text()
        assert 'PANIC' not in text, text
        for mode in range(4):
            if f'DISPLAY-MODE-{mode}\n' in text and mode not in seen:
                qmp('screendump', {'filename':str(work/f'mode-{mode}.png'),'format':'png'})
                seen.add(mode)
        if 'DISPLAY-TIMEOUT-WAIT' in text and text.count('DISPLAY-REVERTED') == 2:
            qmp('screendump', {'filename':str(work/'timeout-restored.png'),'format':'png'})
            break
        if process.poll() is not None: raise AssertionError(process.stderr.read().decode())
        time.sleep(.05)
    else: raise AssertionError(log.read_text())
    assert seen == {0,1,2,3}, seen
finally:
    process.terminate(); process.wait(timeout=5)
run(image, work, 'display-reboot', 'DISPLAY-PERSISTENCE-PASS', seconds=25)
print('Four display modes, Enter/Escape, 15-second auto-revert and reboot persistence passed.', flush=True)
