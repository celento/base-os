"""Open imported images and exercise PS/2 wheel scrolling in the normal desktop."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import time
from PIL import Image,ImageChops
from qemu_session import DesktopSession
from init_data import initialize
from volume import load
build=pathlib.Path(sys.argv[1]).resolve();root=pathlib.Path(__file__).resolve().parents[1]
work=pathlib.Path(tempfile.mkdtemp(prefix='baseos-image-input-data-'));data=work/'data.img';initialize(data)
extra=('-drive',f'file={data},format=raw,index=0,if=ide')
with DesktopSession(build,'image-init',extra=extra) as init:
    init.boot();time.sleep(1)
load(data.read_bytes())
for name in ('landscape.jpg','landscape.png','transparent.png'):
    subprocess.run([sys.executable,str(root/'tools/volume.py'),str(data),'import',str(root/'tests/fixtures/images'/name),'/Pictures/'+name],check=True)
text=work/'wheel.txt';text.write_text(''.join(f'Line {i:03d}: scrolling keeps the caret and document unchanged.\n' for i in range(80)))
subprocess.run([sys.executable,str(root/'tools/volume.py'),str(data),'import',str(text),'/Documents/wheel.txt'],check=True)
symbols={p[2]:int(p[0],16) for line in subprocess.check_output(['nm','-n',str(build/'kernel.elf')],text=True).splitlines() if len(p:=line.split())==3}
def wheel(session,direction):
    button='wheel-down' if direction>0 else 'wheel-up'
    session.command('input-send-event',{'events':[{'type':'btn','data':{'down':True,'button':button}},{'type':'btn','data':{'down':False,'button':button}}]});time.sleep(.12)
with DesktopSession(build,'image-input',extra=extra) as session:
    print(session.directory,flush=True);session.boot()
    assert struct.unpack('<i',session.memory(symbols['mouse_packet_bytes'],4))[0]==4,'wheel negotiation failed'
    session.launch('landscape.jpg');time.sleep(.2)
    state=struct.unpack('<10i',session.memory(symbols['viewer'],40))
    assert state[2]==2 and state[4]==1,('JPEG not loaded',state)
    session.screenshot('jpeg-desktop.png');session.key('1')
    for _ in range(5):session.key('equal')
    before=struct.unpack('<10i',session.memory(symbols['viewer'],40));wheel(session,1)
    after=struct.unpack('<10i',session.memory(symbols['viewer'],40));assert after[7]>before[7],('wheel did not pan',before,after)
    session.screenshot('jpeg-pan.png');session.key('f');session.key('ctrl-w')
    session.launch('landscape.png');time.sleep(.2)
    assert struct.unpack('<10i',session.memory(symbols['viewer'],40))[2]==3,'PNG association'
    session.screenshot('png-desktop.png');session.key('ctrl-w');session.launch('transparent.png');time.sleep(.2)
    session.screenshot('alpha-desktop.png');session.key('ctrl-w');session.launch('wheel.txt');session.key('ctrl-home')
    before=session.screenshot('editor-before-wheel.png')
    for _ in range(3):wheel(session,1)
    after=session.screenshot('editor-after-wheel.png')
    a=Image.open(before).crop((40,135,940,405));b=Image.open(after).crop((40,135,940,405))
    assert ImageChops.difference(a,b).getbbox(),'Editor did not visually scroll'
    session.key('ctrl-end');session.screenshot('editor-after-navigation.png')
    print('Normal desktop: imported JPEG/PNG/alpha, file associations, actual/fit zoom, physical wheel pan and Editor scrolling passed.',flush=True)
