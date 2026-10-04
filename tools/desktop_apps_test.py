"""Normal-kernel PS/2 test of Browser and Media Player desktop integration."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import threading
import time
import http.server
from browser_test import Fixture
from layout import constants
from update_image import install_kernel

build=pathlib.Path(sys.argv[1]).resolve();work=pathlib.Path(tempfile.mkdtemp(prefix='baseos-desktop-apps-'))
print(work,flush=True);c=constants();data=bytearray(c['DISK_SECTORS']*512)
data[:512]=(build/'boot.bin').read_bytes();install_kernel(data,(build/'kernel.bin').read_bytes(),c)
image=work/'disk.img';image.write_bytes(data);log=work/'serial.log'
symbols={parts[2]:int(parts[0],16) for line in subprocess.check_output(['nm','-n',str(build/'kernel.elf')],text=True).splitlines() if len(parts:=line.split())==3}
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture)
threading.Thread(target=server.serve_forever,daemon=True).start()
p=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-drive',f'file={image},format=raw,index=0,if=floppy',
    '-nic','user,model=rtl8139','-audiodev',f'wav,id=out,path={work}/desktop-audio.wav','-device','sb16,audiodev=out',
    '-display','none','-serial',f'file:{log}','-qmp','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
def qmp(name,args=None):
    p.stdin.write(json.dumps({'execute':name,'arguments':args or {}}).encode()+b'\n');p.stdin.flush()
    while True:
        r=json.loads(p.stdout.readline())
        if 'error' in r:raise AssertionError(r)
        if 'return' in r:return r['return']
def key(k):
    qmp('human-monitor-command',{'command-line':'sendkey '+k+' 25'});time.sleep(.065)
def type_text(text):
    mapping={' ':'spc','/':'slash','.':'dot','-':'minus',':':'shift-semicolon','_':'shift-minus'}
    for ch in text:key(mapping.get(ch,'shift-'+ch.lower() if ch.isupper() else ch))
def launch(name):
    key('ctrl-spc');type_text(name);key('ret');time.sleep(.15)
def read(address,size):
    path=work/'memory.bin';qmp('pmemsave',{'val':address,'size':size,'filename':str(path)});return path.read_bytes()
def until(test,message,seconds=20):
    deadline=time.monotonic()+seconds
    while not test():
        if p.poll() is not None:raise AssertionError(p.stderr.read().decode())
        if log.exists() and 'PANIC:' in log.read_text():raise AssertionError(log.read_text())
        if time.monotonic()>deadline:raise AssertionError(message)
        time.sleep(.05)
def audio_state():return struct.unpack_from('<i',read(symbols['status'],4))[0]
def browser_contains(text):return text.encode() in read(c['BROWSER_BASE'],c['BROWSER_CAPACITY'])
def shot(name):qmp('screendump',{'filename':str(work/name),'format':'png'})
try:
    assert p.stdout.readline();qmp('qmp_capabilities')
    until(lambda:log.exists() and 'DESKTOP' in log.read_text(),'desktop boot',30);time.sleep(1)
    launch('browser');key('ctrl-l');type_text(f'http://10.0.2.2:{server.server_port}/index');key('ret')
    until(lambda:'/index' in Fixture.paths and browser_contains('BaseOS live HTTP'),'Browser HTTP page')
    shot('browser-desktop.png');geometry=read(symbols['wins'],64)
    key('ctrl-l');type_text(f'http://10.0.2.2:{server.server_port}/next');key('ret')
    until(lambda:'/next' in Fixture.paths and browser_contains('Next live page'),'Browser second page')
    before=Fixture.paths.count('/index');key('alt-left')
    until(lambda:Fixture.paths.count('/index')>before,'Alt+Left did not navigate')
    assert read(symbols['wins'],24)==geometry[:24],'Browser history shortcut changed window geometry'
    launch('media player');key('ret');until(lambda:audio_state()==2,'player start')
    key('spc');until(lambda:audio_state()==3,'player pause')
    shot('player-paused-desktop.png')
    launch('editor');type_text('Apps remain usable while audio is paused.')
    launch('media player');key('spc');until(lambda:audio_state()==4,'player resume/finish')
    key('ret');until(lambda:audio_state()==2,'player replay');key('ctrl-w')
    until(lambda:audio_state()==0,'player close did not stop audio')
    launch('browser');key('ctrl-l');type_text('file:///Documents/welcome.html');key('ret')
    until(lambda:browser_contains('BaseOS guide'),'local HTML association')
    shot('browser-local-and-editor.png')
    assert (work/'desktop-audio.wav').stat().st_size>10000,'no actual audio capture'
    (work/'requests.json').write_text(json.dumps(Fixture.paths,indent=2))
    print('Desktop launcher, Browser HTTP/history/local file, Media Player pause/resume/replay/close, and concurrent Editor passed.',flush=True)
finally:
    if p.poll() is None:
        try:shot('final-desktop.png')
        except Exception:pass
        p.terminate()
    p.wait(timeout=5);server.shutdown();server.server_close()
