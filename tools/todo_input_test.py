"""Normal Todo keyboard/mouse persistence on a disposable occupied IDE volume.

No debugger, guest-memory reads, pause or guest-call injection. File assertions
inspect only the stopped image; screenshots retain the visible status boundary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import time
from PIL import Image
from volume import data_layout, data_marker, encode_snapshot, load, resolve
from writer_pdf_input_test import PdfSession

class TodoSession(PdfSession):
    def shutdown(self):
        self.key('f10'); self.key('right'); self.key('right'); self.key('up'); self.key('ret')
        limit = time.monotonic() + 90
        while self.process.poll() is None and time.monotonic() < limit:
            time.sleep(.05)
        assert self.process.poll() is not None, 'Safe System Shutdown did not finish'

def make_disk(path):
    layout = data_layout()
    nodes = {0: dict(name='', parent=-1, directory=1, app=0, modified=0, data=b''),
             1: dict(name='Documents', parent=0, directory=1, app=0, modified=0, data=b'')}
    for i, size in enumerate((2<<20, 2<<20, 2<<20, 1<<20)):
        nodes[i+2] = dict(name=f'payload{i}.bin', parent=1, directory=0, app=0, modified=1,
                          data=bytes([17+i])*size)
    header, payload = encode_snapshot(nodes, layout, 1)
    data = bytearray(layout.sectors*512); data[:512] = data_marker()
    offset = layout.lbas[0]*512
    data[offset:offset+512] = header.ljust(512,b'\0')
    data[offset+512:offset+512+len(payload)] = payload
    path.write_bytes(data); assert len(load(data)[2]) == len(nodes)
    return {n['name']: hashlib.sha256(n['data']).hexdigest() for n in nodes.values() if not n['directory']}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=Path)
    args=parser.parse_args();build=args.build.resolve()
    work=Path(tempfile.mkdtemp(prefix='baseos-todo-input-'));print(work,flush=True)
    disk=work/'todo-data.img';payload_hashes=make_disk(disk)
    captures=[];names=[f'task{i:02}' for i in range(1,13)]
    extra=['-drive',f'file={disk},format=raw,index=0,if=ide','-nic','none']
    with TodoSession(build,'todo-first',extra=extra) as guest:
        guest.boot();guest.launch('settings');guest.key('1');guest.key('ret');guest.key('ctrl-w')
        guest.launch('todo');guest.click(260,452)
        for name in names:guest.text(name);guest.key('ret')
        guest.click(202,141);guest.click(202,405)
        captures.append(str(guest.screenshot('todo-twelve-rows-and-status.png')))
        guest.click(260,452);guest.text('kept draft')
        guest.key('ctrl-w');guest.launch('todo')
        captures.append(str(guest.screenshot('todo-reopened-private-draft.png')))
        guest.key('ret') # Full list must retain the unsubmitted draft.
        captures.append(str(guest.screenshot('todo-full-list-keeps-draft.png')))
        guest.click(548,454);guest.key('ctrl-s')
        captures.append(str(guest.screenshot('todo-explicit-save-status.png')))
        guest.key('ctrl-w');guest.shutdown()
        shutil.copyfile(guest.log,work/'first-serial.log')
    nodes=load(disk.read_bytes())[2]
    expected=b''.join((b'x' if i in (0,11) else b' ')+name.encode()+b'\n' for i,name in enumerate(names))
    assert nodes[resolve(nodes,'/prefs/todo')]['data']==expected
    for name,digest in payload_hashes.items():
        assert hashlib.sha256(nodes[resolve(nodes,'/Documents/'+name)]['data']).hexdigest()==digest
    with TodoSession(build,'todo-reboot',extra=extra) as guest:
        guest.boot();guest.launch('todo')
        shot=guest.screenshot('todo-reboot-twelve-tasks.png');captures.append(str(shot))
        assert Image.open(shot).size==(800,600)
        guest.shutdown();shutil.copyfile(guest.log,work/'reboot-serial.log')
    assert load(disk.read_bytes())[2][resolve(load(disk.read_bytes())[2],'/prefs/todo')]['data']==expected
    result=dict(passed=True,build=str(build),kernel_sha256=hashlib.sha256((build/'kernel.bin').read_bytes()).hexdigest(),
                disk=str(disk),screenshots=captures,accepted_tasks=12,unsubmitted_draft_persisted=False,
                exact_task_bytes=expected.decode(),payload_hashes=payload_hashes,
                checks=['normal PS/2 add/toggle/Save/Ctrl+S/close/reopen',
                        'all twelve rows and separate status/footer captured at800x600',
                        'full-list draft retained in-session, not implicitly accepted at shutdown',
                        'safe Shutdown and exact stopped-volume task bytes',
                        'unchanged7MiB payloads and normal reboot'])
    (work/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)

if __name__=='__main__':main()
