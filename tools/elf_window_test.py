"""ELF program windows on the production desktop (BOS_SYS_WINDOW_*).

python3 tools/elf_window_test.py build
Needs build/toolchain, mkfs.fat and mtools. Starts examples/elf/window.c from
Terminal and checks that its true-color surface reaches the screen, that
keys, clicks and relative mouse motion arrive as events, that a resize
grows the window and that Alt+F4 closes it. Screenshots stay in the printed
directory.
"""
import pathlib
import subprocess
import sys
import tempfile
import time

from PIL import Image

from elf_libc_test import TOOLCHAIN
from elf_runtime_test import ROOT
from fat_volume_test import ENV
from pointer_motion import rel_events
from qemu_session import DesktopSession

SURFACE = (0x12, 0x34, 0x56)


def surface_box(path):
    """Bounding box of the program's surface color on a screenshot."""
    image = Image.open(path).convert('RGB')
    w, h = image.size
    data = image.load()
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if data[x, y] == SURFACE:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def run(build):
    gcc = TOOLCHAIN / 'i686-baseos-gcc'
    if not gcc.exists():
        raise SystemExit('Build the toolchain first: python3 toolchain/build.py')
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-window-'))
    program = work / 'window'
    subprocess.run([str(gcc), '-O2', '-Wall', '-I', str(ROOT), str(ROOT / 'examples/elf/window.c'),
                    '-o', str(program)], check=True)
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', str(fat), str(64 * 1024)], check=True, capture_output=True)
    subprocess.run(['mcopy', '-i', str(fat), str(program), '::WINDOW'], check=True, env=ENV)
    with DesktopSession(build, 'elf-window',
                        extra=('-m', '512M', '-boot', 'a',
                               '-drive', f'file={fat},format=raw,if=ide,index=2')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        log = lambda: session.log.read_text(errors='replace')
        session.boot()
        session.launch('terminal')
        session.text('start /disk1/WINDOW')
        session.key('ret')
        session.wait(lambda: 'WIN created' in log(), 'window created')
        session.wait(lambda: 'WIN focus in' in log(), 'focus in')
        time.sleep(.5)
        box = surface_box(session.screenshot('window.png'))
        assert box, 'surface color not on screen'
        x0, y0, x1, y1 = box
        print('surface at', box, flush=True)
        assert 300 <= x1 - x0 + 2 <= 322 and 180 <= y1 - y0 + 2 <= 202, box

        session.key('a')
        session.wait(lambda: 'WIN key down 30 97' in log(), 'key a down')
        session.wait(lambda: 'WIN key up 30' in log(), 'key a up')

        # The pointer starts at the screen center; click inside the surface.
        w, h = Image.open(session.directory / 'window.png').size
        target = (x0 + 40, y0 + 30)
        for batch in rel_events(target[0] - w // 2, target[1] - h // 2):
            session.command('input-send-event', {'events': batch})
        time.sleep(.2)
        for down in (True, False):
            session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.1)
        session.wait(lambda: 'WIN mouse down 1 at' in log() and 'WIN mouse up 1 at' in log(), 'click')
        line = [l for l in log().splitlines() if 'WIN mouse down 1 at' in l][0]
        cx, cy = map(int, line.rsplit(' ', 1)[1].split(','))
        assert abs(cx - 40) <= 2 and abs(cy - 30) <= 2, line

        session.key('m')
        time.sleep(.3)
        for batch in rel_events(30, 0):
            session.command('input-send-event', {'events': batch})
        session.wait(lambda: 'WIN motion ' in log(), 'relative motion')
        session.key('m')

        session.key('r')
        session.wait(lambda: 'WIN resized 400 300' in log(), 'resize')
        time.sleep(.5)
        box = surface_box(session.screenshot('resized.png'))
        assert box and 390 <= box[2] - box[0] + 2 <= 402, box

        session.key('alt-f4')
        session.wait(lambda: 'WIN close' in log(), 'close event')
        session.wait(lambda: 'exited status 0' in log(), 'program exit')
        time.sleep(.5)
        assert surface_box(session.screenshot('closed.png')) is None, 'window still shown'
        session.text('echo desktop still answers')
        session.key('ret')
        time.sleep(.3)
    print('ELF_WINDOW_PASS: true-color surface, keys, clicks, relative mouse, resize and close.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
