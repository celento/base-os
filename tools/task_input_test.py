"""Exercise terminal-owned native tasks through the normal kernel's PS/2 UI."""
import pathlib
import sys
import tempfile
import time
from qemu_session import DesktopSession
from init_data import initialize
from volume import load,resolve
build=pathlib.Path(sys.argv[1]);work=pathlib.Path(tempfile.mkdtemp(prefix='baseos-task-input-data-'))
data=work/'data.img';initialize(data)
with DesktopSession(build,'task-input',extra=('-drive',f'file={data},format=raw,index=0,if=ide')) as session:
    print(session.directory,flush=True);session.boot();session.launch('terminal')
    session.text('start /Programs/counter.bex');session.key('ret');time.sleep(3.2);session.key('s')
    session.key('ctrl-n');session.text('start /Programs/counter.bex');session.key('ret');time.sleep(.3)
    for _ in range(5):session.key('equal')
    session.key('s')
    session.key('ctrl-n');session.text('echo Desktop remains responsive');session.key('ret');time.sleep(.2)
    session.key('alt-tab');session.key('s');session.screenshot('native-counters.png')
    session.key('ctrl-m');session.key('alt-tab');session.key('ctrl-c')
    session.text('echo stopped-ok');session.key('ret')
    session.screenshot('native-stop.png')
    session.key('ctrl-w');session.key('ctrl-w');session.key('alt-tab');session.key('s');session.key('ctrl-w')
    session.launch('terminal');session.text('start /Programs/counter.bex');session.key('ret');time.sleep(.25);session.key('q')
    time.sleep(.25);session.text('echo restart-ok');session.key('ret');time.sleep(2)
    memory=session.memory(session.layout['APPS_BASE']+0x300000,0xc0000)
    assert b'Native task finished.\x00' in memory,'Q did not end task cleanly'
    assert b'restart-ok\x00' in memory,'command input did not return after task exit'
    session.screenshot('native-restart.png')
slot,generation,nodes=load(data.read_bytes())
one=int(nodes[resolve(nodes,'/Documents/counter-1.txt')]['data'])
two=int(nodes[resolve(nodes,'/Documents/counter-2.txt')]['data'])
assert one>=3 and two>=50 and one!=two,(one,two)
print(f'Normal desktop: two long-running tasks, independent saves ({one}/{two}), minimize, Ctrl+C, close, restart and Q exit passed.',flush=True)
