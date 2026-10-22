"""DOSBox Staging on BaseOS: runs a real-mode DOS program on the desktop.

python3 tools/dosbox_test.py build
Needs the ports built with DOSBox (python3 ports/build.py), nasm, mkfs.fat
and mtools. Makes the make run-dos disk with tools/dos_disk.py holding
examples/dos/vgatest.asm, starts DOSBox from Terminal, types VGATEST at the
C: prompt and checks that the program's VGA mode 13h screen reaches the
desktop, that a key press reaches the program, that the file it writes
lands on the disk and that EXIT closes DOSBox cleanly.
"""
import pathlib
import subprocess
import sys
import tempfile
import time

from PIL import Image

from fat_volume_test import ENV
from qemu_session import DesktopSession

ROOT = pathlib.Path(__file__).resolve().parents[1]
# VGA DAC entry 1 is set to 6-bit (4, 13, 21); DOSBox widens it to 8 bits.
SURFACE = tuple((v << 2) | (v >> 4) for v in (4, 13, 21))


def surface_box(path, color=SURFACE, tolerance=3):
    image = Image.open(path).convert('RGB')
    w, h = image.size
    data = image.load()
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if all(abs(a - b) <= tolerance for a, b in zip(data[x, y], color)):
                xs.append(x)
                ys.append(y)
    if len(xs) < 100:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def fat_image(work):
    """The disk make run-dos uses, made by tools/dos_disk.py."""
    apps = work / 'apps'
    apps.mkdir()
    subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'examples/dos/vgatest.asm'),
                    '-o', str(apps / 'VGATEST.COM')], check=True)
    fat = work / 'dos.img'
    subprocess.run([sys.executable, str(ROOT / 'tools/dos_disk.py'), str(fat), str(apps)], check=True)
    return fat


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-dosbox-'))
    fat = fat_image(work)
    with DesktopSession(build, 'dosbox',
                        extra=('-m', '512M', '-boot', 'a',
                               '-drive', f'file={fat},format=raw,if=ide,index=2')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        log = lambda: session.log.read_text(errors='replace')
        session.boot()
        session.launch('terminal')
        session.text('start /disk1/DOSBOX/DOSBOX')
        session.key('ret')
        # DOSBox mounts /DOS as C: and waits at the prompt; type the program.
        time.sleep(25)
        session.screenshot('prompt.png')
        session.text('VGATEST')
        session.key('ret')
        started = time.monotonic()
        box = None
        while time.monotonic() - started < 180 and not box:
            if 'exited status' in log():
                break
            time.sleep(2)
            box = surface_box(session.screenshot('dosbox.png'))
        assert box, 'mode 13h screen not on the desktop\n' + log()[-3000:]
        print(f'VGA screen at {box} after {time.monotonic() - started:.0f} s', flush=True)
        x0, y0, x1, y1 = box
        assert x1 - x0 >= 300 and y1 - y0 >= 180, box

        session.key('spc')
        time.sleep(2)
        session.text('exit')
        session.key('ret')
        session.wait(lambda: 'exited status' in log(), 'DOSBox exit', seconds=60)
        assert 'exited status 0' in log(), log()[-3000:]
        time.sleep(.5)
        assert surface_box(session.screenshot('closed.png')) is None, 'DOSBox window still shown'
    result = subprocess.run(['mtype', '-i', str(fat), '::DOS/RESULT.TXT'], env=ENV,
                            capture_output=True, text=True)
    assert 'VGATEST OK' in result.stdout, result
    print('DOSBOX_PASS: DOS program ran in VGA mode 13h, read a key, wrote a file and exited.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
