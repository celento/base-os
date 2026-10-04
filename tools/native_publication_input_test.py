"""Production desktop frame-publication check with normal SDK app and PS/2 input.

Only screenshots observe the running guest: no debugger, guest-memory access,
injected kernel calls, or faults. All disks are disposable.
"""
import argparse
import json
from pathlib import Path
import tempfile
import time
import numpy as np
from PIL import Image
from build_app import build as build_app
from init_data import initialize
from qemu_session import DesktopSession
from volume import data_layout, encode_snapshot

ROOT=Path(__file__).resolve().parents[1]
COLORS={0:(0,0,0),4:(216,58,58),6:(242,201,76),8:(42,167,200),10:(168,74,192),15:(255,255,255)}

def fixture(directory,profile):
    app=directory/'frame-probe.bex';build_app(ROOT/'tests/native_publication_app.c',app)
    disk=directory/(profile+'.img');initialize(disk,profile=profile);layout=data_layout(profile)
    nodes={0:dict(parent=-1,name='',directory=1,app=0,data=b'',modified=0),
           1:dict(parent=0,name='Programs',directory=1,app=0,data=b'',modified=0),
           2:dict(parent=1,name='frame-probe.bex',directory=0,app=0,data=app.read_bytes(),modified=1)}
    header,payload=encode_snapshot(nodes,layout,1);raw=bytearray(disk.read_bytes());offset=layout.lbas[0]*512
    raw[offset:offset+512]=header.ljust(512,b'\0');raw[offset+512:offset+512+len(payload)]=payload
    disk.write_bytes(raw);return disk

class Session(DesktopSession):
    def memory(self,*args,**kwargs):
        raise AssertionError('Guest-memory observation is forbidden in this test')
    def frame(self,name=None):
        path=self.directory/(name or 'latest.png');self.command('screendump',{'filename':str(path),'format':'png'})
        return np.array(Image.open(path).convert('RGB'))

def expected(phase,width,height,target_width,target_height):
    image=np.full((height,width,3),COLORS[8 if phase else 4],dtype=np.uint8)
    image[:8]=COLORS[10];image[-8:]=COLORS[6]
    for bit in range(8):image[16:24,bit*8:(bit+1)*8]=COLORS[15 if phase&(1<<bit) else 0]
    return image[(np.arange(target_height)*height//target_height)[:,None],
                 (np.arange(target_width)*width//target_width)[None,:]]

def locate(image):
    mask=np.all(image==COLORS[10],axis=2)
    for y in range(image.shape[0]-100):
        xs=np.flatnonzero(mask[y]);
        for x in xs:
            if x and mask[y,x-1]:continue
            for width,height in ((160,100),(320,200),(640,400)):
                if x+width>image.shape[1] or y+height>image.shape[0]:continue
                if np.array_equal(image[y:y+height,x:x+width],expected(0,160,100,width,height)):
                    return int(x),int(y),width,height
    raise AssertionError('Initial complete canvas was not found')

def run(build,profile):
    work=Path(tempfile.mkdtemp(prefix='baseos-publication-'+profile+'-'));disk=fixture(work,profile)
    extra=['-drive',f'file={disk},format=raw,index=0,if=ide']
    if profile=='large':extra+=['-m','128M']
    report=dict(profile=profile,work=str(work),checks=[])
    with Session(build,'native-publication-'+profile,extra=tuple(extra)) as session:
        print('Evidence:',session.directory,flush=True);report['evidence']=str(session.directory)
        session.boot();session.launch('frame-probe.bex');time.sleep(.4)
        x,y,tw,th=locate(session.frame('initial.png'));report['canvas']=[x,y,tw,th]
        phase=0;width=160;height=100;old=expected(phase,width,height,tw,th)
        for key in ('n','y','s','z','r','d','e'):
            session.key(key);began=time.monotonic();old_frames=0;new_frames=0
            if key=='r':width,height=320,200
            if key=='d':width,height=160,100
            target=expected(phase+1,width,height,tw,th)
            # Opening and dismissing the launcher forces unrelated full redraws.
            session.key('ctrl-spc');time.sleep(.15);session.key('esc');time.sleep(.3)
            deadline=began+12
            while time.monotonic()<deadline:
                image=session.frame();canvas=image[y:y+th,x:x+tw]
                if np.array_equal(canvas,old):old_frames+=1
                elif np.array_equal(canvas,target):new_frames+=1;break
                else:
                    Image.fromarray(image).save(session.directory/('incomplete-'+key+'.png'))
                    raise AssertionError(('Incomplete canvas observed',key,old_frames))
                time.sleep(.07)
            assert old_frames>=3 and new_frames,('No staged old/new frames',key,old_frames,new_frames)
            phase+=1;old=target;session.frame('published-'+key+'.png')
            report['checks'].append(dict(boundary=key,old_frames=old_frames,seconds=time.monotonic()-began))
        # Nonzero explicit return keeps its final complete canvas after lifecycle redraw.
        time.sleep(.2);assert np.array_equal(session.frame()[y:y+th,x:x+tw],old)
        session.text('echo explicit return preserved');session.key('ret')
        assert np.array_equal(session.frame()[y:y+th,x:x+tw],old)
        session.key('ctrl-w');session.launch('frame-probe.bex');time.sleep(.3)
        x,y,tw,th=locate(session.frame('reused-slot.png'));old=expected(0,160,100,tw,th)
        session.key('t');time.sleep(.5);assert np.array_equal(session.frame()[y:y+th,x:x+tw],old)
        session.key('ctrl-c');time.sleep(.2)
        assert np.array_equal(session.frame('stopped.png')[y:y+th,x:x+tw],old)
        report['stop_preserved']=True
        assert 'PANIC:' not in session.log.read_text()
    (work/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('build',nargs='?',type=Path,default=Path('build'))
    parser.add_argument('--profile',choices=('default','large'),default='default');args=parser.parse_args()
    run(args.build,args.profile)
