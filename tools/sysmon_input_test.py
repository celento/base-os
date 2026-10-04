"""System Monitor controls through real PS/2 input in the production kernel.

No kernel fixture, fault probe, guest-memory mutation, or persistent disk is used.
ELF symbol reads inspect outcomes; all UI changes are normal keyboard/mouse input.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

from init_data import initialize
from qemu_session import DesktopSession

build = pathlib.Path(sys.argv[1]).resolve()
symbols = {}
sizes = {}
for line in subprocess.check_output(['nm', '-n', '-S', str(build / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 4:
        address, size, _, name = fields
        symbols[name] = int(address, 16)
        sizes[name] = int(size, 16)
required = ('wins', 'mouse_x', 'mouse_y', 'drawn_client', 'drawn_tasks', 'drawn_task_n',
            'drawn_windows', 'drawn_win_n', 'current_tab')
assert all(name in symbols for name in required), 'Build the integrated monitor production kernel first'
# These explicit layout assertions fail safely if the corresponding C structs change.
WIN = struct.Struct('<14i')
TASK = struct.Struct('<24siiIII')
assert sizes['wins'] == WIN.size * 8 and sizes['drawn_tasks'] == TASK.size * 8
work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-monitor-input-data-'))
data = work / 'data.img'
initialize(data)

with DesktopSession(build, 'monitor-input', extra=('-drive', f'file={data},format=raw,index=0,if=ide')) as session:
    print(session.directory, flush=True)

    def integer(name):
        return struct.unpack('<i', session.memory(symbols[name], 4))[0]

    def windows():
        raw = session.memory(symbols['wins'], WIN.size * 8)
        return [dict(zip(('kind', 'x', 'y', 'w', 'h', 'z', 'open', 'seq', 'min',
                          'maximized', 'old_x', 'old_y', 'old_w', 'old_h'),
                         WIN.unpack_from(raw, i * WIN.size))) for i in range(8)]

    def front():
        visible = [(w['z'], i) for i, w in enumerate(windows()) if w['open'] and not w['min']]
        return max(visible)[1] if visible else -1

    def tasks():
        count = integer('drawn_task_n')
        assert 0 <= count <= 8
        raw = session.memory(symbols['drawn_tasks'], TASK.size * count) if count else b''
        result = []
        for i in range(count):
            name, owner, state, started, elapsed, instance = TASK.unpack_from(raw, i * TASK.size)
            result.append(dict(name=name.split(b'\0')[0].decode(), owner=owner, state=state,
                               started=started, elapsed=elapsed, instance=instance))
        return result

    def client():
        return struct.unpack('<4i', session.memory(symbols['drawn_client'], 16))

    def move(x, y):
        deadline = time.monotonic() + 5
        while True:
            mx, my = integer('mouse_x'), integer('mouse_y')
            if (mx, my) == (x, y):
                return
            assert time.monotonic() < deadline, ('mouse move timed out', (mx, my), (x, y))
            dx, dy = max(-100, min(100, x - mx)), max(-100, min(100, y - my))
            events = []
            if dx:
                events.append({'type': 'rel', 'data': {'axis': 'x', 'value': dx}})
            if dy:
                events.append({'type': 'rel', 'data': {'axis': 'y', 'value': dy}})
            session.command('input-send-event', {'events': events})
            time.sleep(.03)

    def click(x, y):
        move(x, y)
        for down in (True, False):
            session.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.08)

    def tab(index):
        x, y, w, _ = client()
        width = (w - 32 - 12) // 3
        click(x + 16 + index * (width + 6) + width // 2, y + 26)
        session.wait(lambda: integer('current_tab') == index, 'monitor tab switch')
        time.sleep(.15)

    def task_button(row, stop=False):
        x, y, w, _ = client()
        # Interior points of the labeled, right-aligned Show and Stop controls.
        click(x + w - 16 - (40 if stop else 160), y + 74 + row * 42 + 11)

    def terminal_memory():
        return session.memory(session.layout['APPS_BASE'] + 0x300000, 0xc0000)

    def monitor():
        session.launch('monitor')
        session.wait(lambda: front() == monitor_owner, 'monitor focus')
        time.sleep(.1)

    try:
        session.boot()
        session.launch('terminal')
        first = front()
        session.text('start /Programs/counter.bex')
        session.key('ret')
        time.sleep(1.1)
        session.key('ctrl-n')
        second = front()
        assert second != first
        session.text('start /Programs/counter.bex')
        session.key('ret')
        time.sleep(.2)
        session.key('ctrl-m')
        assert windows()[second]['min']
        session.launch('monitor')
        monitor_owner = front()
        assert monitor_owner not in (first, second)
        tab(2)
        session.wait(lambda: len(tasks()) == 2, 'two tasks in Monitor')
        initial = tasks()
        assert {task['owner'] for task in initial} == {first, second}
        assert all(task['name'] == 'counter.bex' and task['instance'] and task['state'] in (1, 2)
                   for task in initial), initial
        assert any(task['elapsed'] >= 1 for task in initial), initial
        session.screenshot('monitor-tasks-two.png')

        # Show restores exactly the minimized owning Terminal.
        second_row = next(i for i, task in enumerate(tasks()) if task['owner'] == second)
        task_button(second_row)
        session.wait(lambda: front() == second and not windows()[second]['min'], 'Show terminal restore')
        session.key('equal')
        session.key('s')
        marker = f'Saved /Documents/counter-{second + 1}.txt'.encode()
        session.wait(lambda: marker in terminal_memory(), 'other task responds to input and saves')

        # Enlarged control geometry must stop the original owner, not close either Terminal.
        monitor()
        session.key('alt-ret')
        time.sleep(.2)
        assert client()[2] > 520, 'Monitor did not maximize'
        first_row = next(i for i, task in enumerate(tasks()) if task['owner'] == first)
        task_button(first_row, stop=True)
        session.wait(lambda: len(tasks()) == 1, 'Stop task did not remove its row')
        assert tasks()[0]['owner'] == second
        assert windows()[first]['open'] and windows()[second]['open']
        assert b'Native task stopped.\0' in terminal_memory()
        session.screenshot('monitor-stop-maximized.png')

        # The survivor keeps running and advancing in lifetime after its peer stops.
        before = tasks()[0]['elapsed']
        session.wait(lambda: tasks()[0]['elapsed'] > before, 'surviving task stopped advancing', seconds=5)
        task_button(0)
        session.wait(lambda: front() == second, 'Show survivor terminal')
        session.key('s')
        monitor()
        tab(1)
        # Eight open windows exercise the last Windows row in the minimum-size monitor.
        for _ in range(5):
            session.launch('terminal')
        assert sum(w['open'] for w in windows()) == 8
        monitor()
        session.key('alt-ret')
        time.sleep(.2)
        assert client()[2] == 520
        tab(1)
        session.wait(lambda: integer('drawn_win_n') == 8, 'eight visible Windows rows')
        session.screenshot('monitor-windows-eight.png')

        # Close in the Windows tab remains available and ends only that terminal's task.
        ids = struct.unpack('<8i', session.memory(symbols['drawn_windows'], 32))
        row = ids.index(second)
        x, y, w, _ = client()
        click(x + w - 16 - 35, y + 74 + row * 38 + 16)
        session.wait(lambda: not windows()[second]['open'], 'Windows Close routing')
        assert windows()[first]['open']
        tab(2)
        session.wait(lambda: not tasks(), 'closing terminal did not end native task')
        session.screenshot('monitor-tasks-empty.png')

        # Closing the other windows through Monitor leaves the stopped owner
        # available to demonstrate that Stop preserved its shell rather than closing it.
        for owner, window in enumerate(windows()):
            if window['open'] and owner not in (first, monitor_owner):
                tab(1)
                count = integer('drawn_win_n')
                ids = struct.unpack('<' + 'i' * count, session.memory(symbols['drawn_windows'], count * 4))
                row = ids.index(owner)
                x, y, w, _ = client()
                click(x + w - 16 - 35, y + 74 + row * 38 + 16)
        session.key('ctrl-m')
        w = windows()[first]
        click(w['x'] + w['w'] // 2, w['y'] + 16)
        session.wait(lambda: front() == first, 'stopped owner terminal focus')
        session.text('echo monitor-stop-shell-ok')
        session.key('ret')
        session.wait(lambda: b'monitor-stop-shell-ok\0' in terminal_memory(), 'stopped terminal shell input')
        session.screenshot('monitor-stopped-terminal.png')
        print('Production PS/2 Monitor: real names/state/lifetime, minimized Show, resized Stop, survivor input, eight bounded Windows rows, Close and preserved terminal shell passed.', flush=True)
    except Exception:
        session.screenshot('monitor-failure.png')
        print(session.log.read_text(), flush=True)
        raise
