"""Production desktop .bex launch smoke using ordinary QMP PS/2 input.

A fresh disposable volume gives deterministic seeded Files rows. Uses screenshots
and stopped-volume bytes only: no debugger, guest-memory reads/writes, fault
probes, or guest-injected calls. See native_launch_test.py for the independent
serial-only protected-runtime/reboot fixture.
"""
from pathlib import Path
import sys
import tempfile
import time
from init_data import initialize
from qemu_session import DesktopSession
from volume import load, resolve


def run(build):
    directory = Path(tempfile.mkdtemp(prefix='baseos-native-launch-input-data-'))
    data = directory / 'data.img'
    initialize(data)
    print('Disposable data:', data, flush=True)
    with DesktopSession(build, 'native-launch-input',
                        extra=('-drive', f'file={data},format=raw,index=0,if=ide')) as session:
        print('Screenshots and serial log:', session.directory, flush=True)
        session.boot()
        session.launch('Files')
        # Use the public name filter rather than an obsolete insertion-order
        # row index. Files now sorts names and starts with no selected row.
        session.key('ctrl-f'); session.text('Programs'); session.key('down'); session.key('ret')
        time.sleep(.25)
        session.screenshot('native-files.png')
        session.key('ctrl-f'); session.text('counter.bex'); session.key('down'); session.key('ret')
        time.sleep(3.2)
        session.key('s')
        session.key('ctrl-spc')
        session.text('counter.bex')
        session.screenshot('native-launcher.png')
        session.key('ret')
        time.sleep(.2)
        for _ in range(4):
            session.key('equal')
        session.key('s')
        time.sleep(.3)
        session.screenshot('native-two-tasks.png')
        session.key('ctrl-c')
        session.text('echo stopped cleanly')
        session.key('ret')
        session.screenshot('native-stopped.png')
        session.key('ctrl-w')
        session.launch('notebook.bex')
        time.sleep(.4)
        session.screenshot('native-notebook.png')
        session.key('ctrl-w')
        session.launch('counter.bex')
        time.sleep(.25)
        session.key('q')
        time.sleep(.3)
        session.text('echo launch and exit work')
        session.key('ret')
        session.screenshot('native-finished.png')
        time.sleep(1)
    _, _, nodes = load(data.read_bytes())
    first = int(nodes[resolve(nodes, '/Documents/counter-2.txt')]['data'])
    second = int(nodes[resolve(nodes, '/Documents/counter-3.txt')]['data'])
    assert first >= 3 and second >= 40, (first, second)
    assert nodes[resolve(nodes, '/Documents/sdk-note.txt')]['data'] == (
        b'This note was saved by a protected C application.\n')
    print(f'PS2_FILES_SEARCH_SAVE_STOP_EXIT_PASS: counters {first}/{second}, exact Notebook bytes.', flush=True)


if __name__ == '__main__':
    run(Path(sys.argv[1] if len(sys.argv) > 1 else 'build'))
