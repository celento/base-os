"""SDL 2 programs on BaseOS: the ported SDL with its BaseOS video and audio drivers.

python3 tools/elf_sdl_test.py build
Needs build/toolchain with the ports built (python3 ports/build.py), mkfs.fat
and mtools. Starts examples/elf/sdl_demo.c from Terminal and checks that SDL
picks the BaseOS drivers, that the software renderer reaches the screen, that
keys, text input and clicks arrive as SDL events and that Alt+F4 quits.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

from PIL import Image

from elf_libc_test import TOOLCHAIN
from elf_runtime_test import ROOT
from elf_window_test import surface_box
from fat_volume_test import ENV
from pointer_motion import rel_events
from qemu_session import DesktopSession

SYSROOT = ROOT / 'build/toolchain/sysroot'


def compile_demo(output):
    gcc = TOOLCHAIN / 'i686-baseos-gcc'
    if not (SYSROOT / 'usr/lib/libSDL2.a').exists():
        raise SystemExit('Build the ports first: python3 ports/build.py')
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(SYSROOT),
               PKG_CONFIG_LIBDIR=str(SYSROOT / 'usr/lib/pkgconfig'))
    flags = subprocess.run(['pkg-config', '--static', '--cflags', '--libs', 'sdl2'], env=env,
                           check=True, capture_output=True, text=True).stdout.split()
    subprocess.run([str(gcc), '-O2', '-Wall', str(ROOT / 'examples/elf/sdl_demo.c'), *flags,
                    '-o', str(output)], check=True)


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-sdl-'))
    program = work / 'sdl_demo'
    compile_demo(program)
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', str(fat), str(64 * 1024)], check=True, capture_output=True)
    subprocess.run(['mcopy', '-i', str(fat), str(program), '::SDLDEMO'], check=True, env=ENV)
    with DesktopSession(build, 'elf-sdl',
                        extra=('-m', '512M', '-boot', 'a',
                               '-drive', f'file={fat},format=raw,if=ide,index=2')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        log = lambda: session.log.read_text(errors='replace')
        session.boot()
        session.launch('terminal')
        session.text('start /disk1/SDLDEMO')
        session.key('ret')
        session.wait(lambda: 'SDL ready' in log() or 'SDL init failed' in log()
                     or 'SDL window failed' in log(), 'SDL ready')
        assert 'SDL drivers: video baseos, audio baseos' in log(), log()[-2000:]
        assert 'SDL ready: audio open' in log(), log()[-2000:]
        time.sleep(1)
        box = surface_box(session.screenshot('sdl.png'))
        assert box, 'renderer output not on screen'
        x0, y0, x1, y1 = box
        print('surface at', box, flush=True)
        assert 300 <= x1 - x0 + 2 <= 322 and 220 <= y1 - y0 + 2 <= 242, box

        session.key('a')
        session.wait(lambda: 'SDL key A' in log(), 'key a')
        session.wait(lambda: 'SDL text a' in log(), 'text a')

        w, h = Image.open(session.directory / 'sdl.png').size
        target = (x0 + 40, y0 + 30)
        for batch in rel_events(target[0] - w // 2, target[1] - h // 2):
            session.command('input-send-event', {'events': batch})
        time.sleep(.2)
        for down in (True, False):
            session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.1)
        session.wait(lambda: 'SDL click 1 at' in log(), 'click')
        line = [l for l in log().splitlines() if 'SDL click 1 at' in l][0]
        cx, cy = map(int, line.rsplit(' ', 1)[1].split(','))
        assert abs(cx - 40) <= 2 and abs(cy - 30) <= 2, line

        session.key('alt-f4')
        session.wait(lambda: 'SDL quit' in log(), 'quit event')
        session.wait(lambda: 'exited status 0' in log(), 'program exit')
        frames = [l for l in log().splitlines() if 'SDL frames' in l]
        print(frames[-1] if frames else 'no frame count', flush=True)
        time.sleep(.5)
        assert surface_box(session.screenshot('closed.png')) is None, 'window still shown'
    print('ELF_SDL_PASS: BaseOS SDL drivers, renderer, keys, text, clicks and quit.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
