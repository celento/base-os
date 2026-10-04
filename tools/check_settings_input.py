"""Prepared normal-kernel Settings input check; NEVER RUN before cloud reset.

Requires repo tools on PYTHONPATH. Uses no guest-memory/debugger observations.
The preference bytes are inspected offline only after the guest exits.
"""
import json
from pathlib import Path
import sys
import tempfile
import time
from PIL import Image
from init_data import initialize
from qemu_session import DesktopSession
from volume import load, resolve

build = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix='baseos-settings-input-'))
print(work, flush=True)
disk = work / 'settings-data.img'
initialize(disk)
captures = []

class SettingsSession(DesktopSession):
    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part} for part in key.split('-')], 'hold-time': 40})
        time.sleep(.18)
    def capture(self, name, dimensions):
        screenshot = self.screenshot(name)
        assert Image.open(screenshot).size == dimensions, (name, Image.open(screenshot).size)
        captures.append(str(screenshot))
        return screenshot
    def shutdown(self):
        self.key('f10'); self.key('right'); self.key('right'); self.key('up'); self.key('ret')
        deadline = time.monotonic() + 20
        while self.process.poll() is None and time.monotonic() < deadline:
            time.sleep(.1)
        assert self.process.poll() is not None, 'System Shutdown did not complete'

extra = ['-drive', f'file={disk},format=raw,index=0,if=ide']
with SettingsSession(build, 'settings-first', extra=extra) as guest:
    guest.boot(); guest.launch('settings'); guest.key('1'); guest.key('ret')
    guest.key('ctrl-w'); guest.launch('settings')
    guest.key('right'); guest.key('spc'); guest.key('r'); time.sleep(1)
    guest.capture('settings-saved-minimum-800x600.png', (800, 600))
    guest.key('3'); guest.capture('settings-preview-1280x720.png', (1280, 720))
    guest.key('esc'); guest.capture('settings-reverted-800x600.png', (800, 600))
    guest.key('4'); guest.key('ret'); time.sleep(1)
    guest.capture('settings-kept-1280x800.png', (1280, 800))
    guest.key('2'); guest.capture('settings-timeout-preview.png', (1024, 768))
    time.sleep(17)
    guest.capture('settings-timeout-restored.png', (1280, 800))
    guest.shutdown()
# Inspect only the persisted disk after QEMU exits.
nodes = load(disk.read_bytes())[2]
expected = {'theme': b'1', 'saver': b'0', 'display': b'3'}
for name, value in expected.items():
    assert nodes[resolve(nodes, '/prefs/' + name)]['data'] == value, name
with SettingsSession(build, 'settings-reboot', extra=extra) as guest:
    guest.boot(); guest.launch('settings')
    guest.capture('settings-reboot-restored.png', (1280, 800))
    guest.shutdown()
report = {'build': str(build), 'disk': str(disk), 'preferences': {k: v.decode() for k, v in expected.items()},
          'captures': captures, 'checks': ['minimum layout', 'keyboard choices and retry', 'Keep/Escape',
              '15-second automatic revert', 'normal shutdown', 'offline snapshot bytes', 'reboot dimensions']}
(work / 'results.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2), flush=True)
