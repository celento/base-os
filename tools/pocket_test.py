"""Boot the Pocket shell in QEMU and prove the kernel/shell swap end to end.

Pocket is linked from the same kernel, ui and app objects as the desktop
(make pocket). This drives it with PS/2 keys and mouse clicks, adds a todo,
shuts down from About, then boots the desktop kernel on the same disk and
checks that it sees the item Pocket saved. Images are disposable.
Usage: pocket_test.py [pocket-build] [desktop-build] [--screenshots DIR]
"""
import argparse
import pathlib
import shutil
import struct
import tempfile
import time

from kernel_space_test import symbols
from qemu_session import DesktopSession
from update_image import install_kernel
import pointer_motion

ROOT = pathlib.Path(__file__).resolve().parents[1]
STATUS_H, HEADER_H = 30, 48
APPS = ['Files', 'Editor', 'Settings', 'Clock', 'Todo', '2048', 'Mines', 'Wordle', 'Breakout', 'About']


class PocketSession(DesktopSession):
    def __init__(self, build, **options):
        super().__init__(build, label='pocket', **options)
        self.symbols = symbols(self.build / 'kernel.elf')

    def boot(self):
        self.wait(lambda: 'POCKET-READY\n' in self.serial(), 'first Pocket frame', 90)
        time.sleep(.3)

    def serial(self):
        return self.log.read_text() if self.log.exists() else ''

    def marker(self, text, seconds=10):
        count = self.serial().count(text)
        return lambda: self.wait(lambda: self.serial().count(text) > count, text, seconds)

    def int(self, name):
        return struct.unpack('<i', self.memory(self.symbols[name], 4))[0]

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part} for part in key.split('-')],
                                  'hold-time': 35})
        time.sleep(.12)

    def type(self, text):
        for char in text:
            self.key({' ': 'spc'}.get(char, char))

    def move(self, x, y):
        # PS/2 motion is relative; correct against the shell's own pointer.
        for _ in range(60):
            dx, dy = x - self.int('pocket_mouse_x'), y - self.int('pocket_mouse_y')
            if not dx and not dy:
                return
            events = [{'type': 'rel', 'data': {'axis': axis, 'value': pointer_motion.step(delta, 40)}}
                      for axis, delta in (('x', dx), ('y', dy)) if delta]
            self.command('input-send-event', {'events': events})
            time.sleep(.1)
        raise AssertionError(f'pointer stopped at {self.int("pocket_mouse_x")},'
                             f'{self.int("pocket_mouse_y")}, not {x},{y}')

    def click(self, x, y):
        self.move(x, y)
        for down in (True, False):
            self.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.12)

    def home_tile(self, index):
        # The shell publishes its grid geometry after the first frame.
        x, y = self.int('pocket_grid_x'), self.int('pocket_grid_y')
        w, h, columns = self.int('pocket_cell_w'), self.int('pocket_cell_h'), self.int('pocket_columns')
        return x + index % columns * w + w // 2, y + index // columns * h + h // 2 - 8

    def back_button(self):
        return self.int('phone_x') + 22, self.int('phone_y') + STATUS_H + HEADER_H // 2


def run(pocket, desktop, shots):
    with PocketSession(pocket) as session:
        session.boot()
        log = session.serial()
        assert 'Shell pocket\n' in log and 'DESKTOP-READY' not in log, log
        if shots: shutil.copy(session.screenshot('home.png'), shots / 'pocket-home.png')

        # Keyboard: arrows select a tile, Enter opens it, Esc returns home.
        opened = session.marker('POCKET-OPEN 2048\n')
        for _ in range(APPS.index('2048')): session.key('right')
        session.key('ret'); opened()
        session.key('left'); session.key('up')
        home = session.marker('POCKET-HOME\n')
        session.key('esc'); home()

        # Pointer: tap the Clock tile, then the header's back button.
        opened = session.marker('POCKET-OPEN Clock\n')
        session.click(*session.home_tile(APPS.index('Clock'))); opened()
        if shots: shutil.copy(session.screenshot('clock.png'), shots / 'pocket-clock.png')
        home = session.marker('POCKET-HOME\n')
        session.click(*session.back_button()); home()

        # Todo: an app model shared with the desktop, saved to the same file.
        opened = session.marker('POCKET-OPEN Todo\n')
        session.click(*session.home_tile(APPS.index('Todo'))); opened()
        session.type('pocket item'); session.key('ret')
        session.wait(lambda: session.int('td_n') == 1, 'todo item added'); time.sleep(.3)
        if shots: shutil.copy(session.screenshot('todo.png'), shots / 'pocket-todo.png')
        home = session.marker('POCKET-HOME\n')
        session.key('esc'); home()

        opened = session.marker('POCKET-OPEN About\n')
        session.click(*session.home_tile(APPS.index('About'))); opened()
        if shots: shutil.copy(session.screenshot('about.png'), shots / 'pocket-about.png')
        session.key('ret')
        session.wait(lambda: 'POCKET-SHUTDOWN\n' in session.serial(), 'shutdown from About', 30)
        session.process.wait(timeout=30)
        disk = (session.directory / 'disk.img').read_bytes()

    # Same disk, desktop kernel: the item Pocket saved is there.
    image = bytearray(disk)
    install_kernel(image, (desktop / 'kernel.bin').read_bytes())
    shared = pathlib.Path(tempfile.mkdtemp(prefix='baseos-pocket-shared-')) / 'disk.img'
    shared.write_bytes(image)
    with DesktopSession(desktop, label='pocket-desktop', image=shared) as session:
        session.boot()
        assert 'Shell desktop\n' in session.log.read_text() and 'FS loaded from disk' in session.log.read_text()
        session.launch('todo'); time.sleep(.5)
        count = struct.unpack('<i', session.memory(symbols(desktop / 'kernel.elf')['td_n'], 4))[0]
        assert count == 1, f'desktop sees {count} todo items'
    print('Pocket shell: boot, keys, pointer, todo, shutdown and desktop handoff passed.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pocket', type=pathlib.Path, nargs='?', default=ROOT / 'build/pocket')
    parser.add_argument('desktop', type=pathlib.Path, nargs='?', default=ROOT / 'build')
    parser.add_argument('--screenshots', type=pathlib.Path)
    arguments = parser.parse_args()
    for build in (arguments.pocket, arguments.desktop):
        if not (build / 'kernel.bin').exists():
            raise SystemExit(f'missing {build}/kernel.bin; run make and make pocket first')
    if arguments.screenshots: arguments.screenshots.mkdir(parents=True, exist_ok=True)
    run(arguments.pocket.resolve(), arguments.desktop.resolve(), arguments.screenshots)
