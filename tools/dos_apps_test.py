"""The bundled DOS programs start from the Launcher with no setup.

python3 tools/dos_apps_test.py build [SHOTS]
Needs DOSBox built (python3 ports/build.py), mkfs.fat, mtools and
tesseract. Makes the DOS disk with tools/dos_disk.py, the same way make run
does, and boots with it attached. For each program in tools/dos_apps.py it opens the Launcher,
types the program's name and presses Enter, which opens its .dos launcher in
/Applications. It waits until DOSBox's window shows the program's first screen, saves a
screenshot (into SHOTS when given), and closes the window with Alt+F4.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops

import dos_apps
from qemu_session import DesktopSession

ROOT = pathlib.Path(__file__).resolve().parents[1]
# What each program shows once it runs: words its first screen holds (read
# with tesseract), or None for a full-screen picture with many colors.
EXPECT = {'FED': 'untitled', 'NETHACK': 'are you?', 'FLPYBIRD': None, 'VITETRIS': 'vitetris', 'SUDOKU86': None}
# How each program is closed: Alt+F4 closes the DOSBox window. Vitetris keeps
# DOSBox busy so that it does not see the close request (a second Alt+F4 ends
# it), so it quits with its own Q key instead, which also checks that DOSBox
# closes when the launcher's batch file ends.
QUIT = {'VITETRIS': 'q'}
TASKBAR = 40


def changed_box(before, after):
    """Bounding box of a window-sized change between two screenshots, or None.
    Small changes, such as the clock ticking, do not count."""
    a = Image.open(before).convert('RGB')
    b = Image.open(after).convert('RGB')
    # Leave out the taskbar, where the clock and the window's button change.
    a, b = (image.crop((0, 0, image.width, image.height - TASKBAR)) for image in (a, b))
    box = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v > 24 else 0).getbbox()
    if box and box[2] - box[0] >= 200 and box[3] - box[1] >= 150:
        return box
    return None


def client(path, box):
    """The window's contents, without its title bar and frame."""
    x0, y0, x1, y1 = box
    return Image.open(path).convert('RGB').crop((x0 + 4, y0 + 36, x1 - 4, y1 - 4))


def shows(path, box, expect):
    """Whether the window shows the program's first screen."""
    image = client(path, box)
    if expect is None:
        small = image.resize((160, 120))
        pixels = list(small.getdata())
        lit = sum(1 for p in pixels if max(p) > 40) / len(pixels)
        return len(set(pixels)) >= 64 and lit >= 0.5
    big = image.resize((image.width * 2, image.height * 2))
    scratch = pathlib.Path(path).with_suffix('.ocr.png')
    big.save(scratch)
    text = subprocess.run(['tesseract', str(scratch), '-', '--psm', '6'], capture_output=True, text=True).stdout
    return expect.lower() in text.lower()


def nudge(session):
    """Move the pointer by one pixel and back, so the screen saver (90 s
    without input) stays off while a slow program starts."""
    for value in (1, -1):
        session.command('input-send-event', {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': value}}]})


def run(build, shots=None):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-dos-apps-'))
    fat = work / 'dos.img'
    subprocess.run([sys.executable, str(ROOT / 'tools/dos_disk.py'), str(fat)], check=True)
    if shots:
        shots = pathlib.Path(shots)
        shots.mkdir(parents=True, exist_ok=True)
    with DesktopSession(build, 'dos-apps',
                        extra=('-m', '512M', '-boot', 'a',
                               '-drive', f'file={fat},format=raw,if=ide,index=2')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        log = lambda: session.log.read_text(errors='replace')
        session.boot()
        time.sleep(2)
        for app in dos_apps.APPS:
            name = app['name']
            before = session.screenshot(f'{name}-desktop.png')
            exits = log().count('exited status')
            zeros = log().count('exited status 0')
            # DOSBox logs its arguments first thing: the launcher's batch file.
            started_line = f'arguments: /disk1/DOSBOX/DOSBOX {dos_apps.BIN}/{name}.BAT'
            runs = log().count(started_line)
            session.launch(app['title'])
            session.wait(lambda: log().count(started_line) > runs, f'{app["title"]}: DOSBox did not start',
                         seconds=60)
            # The DOSBox window appears first; the program needs longer.
            session.wait(lambda: changed_box(before, session.screenshot(f'{name}-wait.png')),
                         f'{app["title"]}: no DOSBox window', seconds=120)
            started = time.monotonic()
            while True:
                nudge(session)
                shot = session.screenshot(f'{name}.png')
                box = changed_box(before, shot)
                assert box, f'{app["title"]}: window gone\n' + log()[-3000:]
                if shows(shot, box, EXPECT[name]):
                    break
                assert log().count('exited status') == exits, f'{app["title"]}: DOSBox quit\n' + log()[-3000:]
                assert time.monotonic() - started < 300, f'{app["title"]}: program screen never appeared'
                time.sleep(5)
            print(f'{app["title"]}: running after {time.monotonic() - started:.0f} s, window {box}', flush=True)
            assert box[2] - box[0] >= 300 and box[3] - box[1] >= 200, box
            if shots:
                shutil.copy(shot, shots / f'{name.lower()}.png')
            session.key(QUIT.get(name, 'alt-f4'))
            session.wait(lambda: log().count('exited status') > exits, f'{app["title"]}: DOSBox did not close',
                         seconds=90)
            assert log().count('exited status 0') > zeros, f'{app["title"]}: DOSBox failed\n' + log()[-3000:]
            time.sleep(3)
            assert not changed_box(before, session.screenshot(f'{name}-closed.png')), \
                f'{app["title"]}: window still shown'
    print(f'DOS_APPS_PASS: {len(dos_apps.APPS)} DOS programs started from the Launcher and closed.', flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build', sys.argv[2] if len(sys.argv) > 2 else None)
