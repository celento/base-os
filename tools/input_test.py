"""Exercise real PS/2 input through QEMU, including BASIC's INKEY callback."""
import json,pathlib,subprocess,sys,tempfile,time
from layout import constants
build=pathlib.Path(sys.argv[1]).resolve();d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-input-'));print(d,flush=True)
c=constants();image=bytearray(c['DISK_SECTORS']*512);kernel=(build/'kernel.bin').read_bytes();image[:512]=(build/'boot.bin').read_bytes();image[512:512+len(kernel)]=kernel
path=d/'disk.img';path.write_bytes(image);log=d/'serial.log'
p=subprocess.Popen(['qemu-system-i386','-m','32M','-vga','std','-drive',f'file={path},format=raw,index=0,if=floppy','-display','none','-serial',f'file:{log}','-qmp','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
try:
    assert p.stdout.readline()
    def qmp(name,args=None):
        p.stdin.write(json.dumps(dict(execute=name,arguments=args or {})).encode()+b'\n');p.stdin.flush()
        while True:
            r=json.loads(p.stdout.readline())
            if 'error' in r:raise AssertionError(r)
            if 'return' in r:return r['return']
    qmp('qmp_capabilities')
    deadline=time.monotonic()+30
    while not log.exists() or 'DESKTOP' not in log.read_text():
        assert time.monotonic()<deadline,log.read_text() if log.exists() else 'no boot log';time.sleep(.1)
    time.sleep(1)
    def key(k):
        qmp('human-monitor-command',{'command-line':'sendkey '+k+' 35'});time.sleep(.09)
    def text(s):
        for ch in s:key('shift-'+ch.lower() if ch.isupper() else {' ':'spc','/':'slash','.':'dot','-':'minus'}.get(ch,ch))
    def memory():
        target=d/'terminal.bin';qmp('pmemsave',{'val':c['APPS_BASE']+0x300000,'size':64*81,'filename':str(target)});return target.read_bytes()
    key('ctrl-spc');text('terminal');key('ret');time.sleep(.5)
    text('echo input-check');key('ret');time.sleep(.5)
    assert b'input-check\x00' in memory(),'terminal keyboard input failed'
    text('basic /Programs/demo.bas');key('ret');time.sleep(.25);key('z')
    deadline=time.monotonic()+15
    while b'Program finished.' not in memory():
        assert time.monotonic()<deadline,'BASIC did not finish';time.sleep(.2)
    assert b'122\x00' in memory(),'INKEY did not receive z'
    qmp('screendump',{'filename':str(d/'terminal.png'),'format':'png'})
    print('PS/2 launcher, terminal typing, and BASIC INKEY passed.',flush=True)
except Exception:
    qmp("screendump",{"filename":str(d/"failure.png"),"format":"png"})
    print(log.read_text(),flush=True)
    p.terminate();p.wait(timeout=5);raise
else:
    p.terminate();p.wait(timeout=5)
