"""Capture a disposable QEMU desktop: image output.png [wait-seconds]."""
import json,pathlib,subprocess,sys,tempfile,time
image=pathlib.Path(sys.argv[1]).resolve();output=pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='baseos-capture-') as temp:
    log=pathlib.Path(temp)/'serial'
    p=subprocess.Popen(['qemu-system-i386','-m','32M','-vga','std','-drive',f'file={image},format=raw,index=0,if=floppy','-display','none','-serial',f'file:{log}','-qmp','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        greeting=p.stdout.readline()
        if not greeting:raise RuntimeError(p.stderr.read().decode())
        json.loads(greeting)
        def command(name,args=None):
            p.stdin.write(json.dumps(dict(execute=name,arguments=args or {})).encode()+b'\n');p.stdin.flush()
            while True:
                response=json.loads(p.stdout.readline())
                if 'error' in response:raise RuntimeError(response)
                if 'return' in response:return response['return']
        command('qmp_capabilities')
        time.sleep(float(sys.argv[3]) if len(sys.argv)>3 else 15)
        command('screendump',{'filename':str(output),'format':'png'})
        print(log.read_text());print(output)
    finally:
        p.terminate();p.wait(timeout=5)
