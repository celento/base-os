"""DOSBox Staging on BaseOS: runs a real-mode DOS program on the desktop.

python3 tools/dosbox_test.py build
Needs the ports built with DOSBox (python3 ports/build.py), nasm, mkfs.fat
and mtools. Puts DOSBox with its resources and settings in /DOSBOX on a FAT
disk and examples/dos/vgatest.asm in /DOS, starts DOSBox from Terminal and
checks that the program's VGA mode 13h screen reaches the desktop, that a
key press reaches the program, that the file it writes lands on the disk
and that DOSBox exits cleanly.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image

from fat_volume_test import ENV
from qemu_session import DesktopSession

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOSBOX = ROOT / 'build/ports/dosbox'
# VGA DAC entry 1 is set to 6-bit (4, 13, 21); DOSBox widens it to 8 bits.
SURFACE = tuple((v << 2) | (v >> 4) for v in (4, 13, 21))
AUTOEXEC = """
[autoexec]
mount c /disk1/DOS
c:
VGATEST
exit
"""


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
    if not (DOSBOX / 'DOSBOX').exists():
        raise SystemExit('Build DOSBox first: python3 ports/build.py dosbox')
    tree = work / 'disk'
    shutil.copytree(DOSBOX, tree / 'DOSBOX')
    conf = tree / 'DOSBOX/dosbox-staging.conf'
    text = conf.read_text()
    conf.write_text(text[:text.index('[autoexec]')] + AUTOEXEC.lstrip())
    (tree / 'DOS').mkdir()
    subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'examples/dos/vgatest.asm'),
                    '-o', str(tree / 'DOS/VGATEST.COM')], check=True)
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', str(fat), str(128 * 1024)], check=True, capture_output=True)
    subprocess.run(['mcopy', '-s', '-i', str(fat), *map(str, tree.iterdir()), '::'], check=True, env=ENV)
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
