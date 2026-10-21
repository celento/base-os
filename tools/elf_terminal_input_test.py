"""Production desktop Terminal launching ELF programs, using PS/2 input.

python3 tools/elf_terminal_input_test.py build
Seeds a disposable data volume with the freestanding tests/elf_program.c ELF
and two argument files, starts it twice from Terminal, and checks the serial
exit log and the Terminal scrollback (read from the stopped guest's app
arena). Screenshots stay in the printed directory.
"""
import pathlib
import sys
import tempfile
import time

from elf_runtime_test import ROOT, tool
from init_data import initialize
from layout import constants
from qemu_session import DesktopSession
from volume import DATA_LAYOUT, encode_snapshot, load
import subprocess


def make_fixture(directory):
    program = directory / 'elf_program'
    subprocess.run([tool('gcc'), '-m32', '-static', '-nostdlib', '-ffreestanding', '-fno-pie',
                    '-no-pie', '-fno-stack-protector', '-Os', '-I', str(ROOT),
                    '-Wl,-Ttext-segment=0x40100000', '-Wl,-z,max-page-size=4096',
                    '-Wl,--build-id=none', '-o', str(program), str(ROOT / 'tests/elf_program.c')],
                   check=True)
    disk = directory / 'elf-data.img'
    initialize(disk)
    data = bytearray(disk.read_bytes())
    nodes = {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0),
        2: dict(parent=1, name='elf-check', directory=0, app=0, data=program.read_bytes(), modified=0),
        3: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0),
        4: dict(parent=3, name='hello', directory=0, app=0, data=b'\n', modified=0),
        5: dict(parent=3, name='threads', directory=0, app=0, data=b'\n', modified=0),
    }
    header, payload = encode_snapshot(nodes, DATA_LAYOUT, 1)
    start = DATA_LAYOUT.lbas[0] * 512
    data[start:start + 512] = header.ljust(512, b'\0')
    data[start + 512:start + 512 + len(payload)] = payload
    assert len(load(data)[2]) == len(nodes)
    disk.write_bytes(data)
    return disk


def run(build):
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-terminal-data-'))
    disk = make_fixture(directory)
    c = constants()
    with DesktopSession(build, 'elf-terminal',
                        extra=('-drive', f'file={disk},format=raw,index=0,if=ide')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        session.boot()
        session.launch('terminal')
        session.text('start /Programs/elf-check /Documents/hello')
        session.key('ret')
        session.wait(lambda: 'uproc: pid 1 exited status 7' in session.log.read_text(), 'hello exit')
        session.text('start /Programs/elf-check /Documents/threads')
        session.key('ret')
        session.wait(lambda: 'uproc: pid 2 exited status 0' in session.log.read_text(), 'threads exit')
        time.sleep(.5)
        session.text('echo desktop still answers')
        session.key('ret')
        time.sleep(.5)
        session.screenshot('elf-terminal.png')
        arena = session.memory(c['APPS_BASE'] + 0x300000, 0x40000)
    for needle in (b'ELF hello from ring 3', b'argc 2', b'Program exited with status 7.',
                   b'ELF threads ok', b'Program exited with status 0.', b'desktop still answers'):
        assert needle in arena, needle
    print('ELF_TERMINAL_INPUT_PASS: Terminal start, output, exit status, desktop responsive.', flush=True)


if __name__ == '__main__':
    run(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'build'))
