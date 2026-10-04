"""Small normal-kernel QMP/PS2 harness for reproducible desktop checks."""
import json
import pathlib
import subprocess
import tempfile
import time
from layout import constants
from update_image import install_kernel

class DesktopSession:
    def __init__(self,build,label='desktop',extra=(),image=None):
        self.build=pathlib.Path(build).resolve();self.layout=constants()
        self.directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-'+label+'-'))
        self.log=self.directory/'serial.log'
        if image is None:
            image=self.directory/'disk.img';data=bytearray(self.layout['DISK_SECTORS']*512)
            data[:512]=(self.build/'boot.bin').read_bytes()
            install_kernel(data,(self.build/'kernel.bin').read_bytes(),self.layout);image.write_bytes(data)
        self.stderr=(self.directory/'stderr.log').open('w')
        self.process=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-drive',
            f'file={image},format=raw,index=0,if=floppy','-display','none','-serial',f'file:{self.log}',
            '-qmp','stdio','-no-reboot',*extra],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.stderr)
        if not self.process.stdout.readline():raise RuntimeError('QEMU failed to start')
        self.command('qmp_capabilities')
    def command(self,name,arguments=None):
        self.process.stdin.write(json.dumps({'execute':name,'arguments':arguments or {}}).encode()+b'\n');self.process.stdin.flush()
        while True:
            line=self.process.stdout.readline()
            if not line:raise RuntimeError('QEMU stopped: '+self.log.read_text())
            response=json.loads(line)
            if 'error' in response:raise RuntimeError(response)
            if 'return' in response:return response['return']
    def wait(self,predicate,message,seconds=30):
        deadline=time.monotonic()+seconds
        while not predicate():
            if self.process.poll() is not None:raise AssertionError('QEMU exited: '+message)
            if self.log.exists() and 'PANIC:' in self.log.read_text():raise AssertionError(self.log.read_text())
            if time.monotonic()>deadline:raise AssertionError(message)
            time.sleep(.05)
    def boot(self):
        self.wait(lambda:self.log.exists() and 'DESKTOP' in self.log.read_text(),'desktop boot',90);time.sleep(.5)
    def key(self,key):
        self.command('human-monitor-command',{'command-line':'sendkey '+key+' 25'});time.sleep(.065)
    def text(self,text):
        mapping={' ':'spc','/':'slash','.':'dot','-':'minus',':':'shift-semicolon','_':'shift-minus','\n':'ret'}
        for char in text:self.key(mapping.get(char,'shift-'+char.lower() if char.isupper() else char))
    def launch(self,name):
        self.key('ctrl-spc');self.text(name);self.key('ret');time.sleep(.15)
    def memory(self,address,size):
        target=self.directory/'memory.bin';self.command('pmemsave',{'val':address,'size':size,'filename':str(target)});return target.read_bytes()
    def screenshot(self,name):
        target=self.directory/name;self.command('screendump',{'filename':str(target),'format':'png'});return target
    def close(self):
        try:
            if self.process.poll() is None:
                self.process.terminate()
                try:self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            else:self.process.wait(timeout=5)
        finally:
            for stream in (self.process.stdin,self.process.stdout):
                if stream:stream.close()
            self.stderr.close()
    def __enter__(self):return self
    def __exit__(self,*_):self.close()
