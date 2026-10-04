"""Exercise native tasks with real MP3/SB16 output and HTTP in disposable QEMU."""
import array, http.server, math, pathlib, shutil, subprocess, sys, tempfile, threading, time, wave
from layout import constants
from update_image import install_kernel
from browser_test import screenshot

ROOT=pathlib.Path(__file__).resolve().parents[1]
def tool(name):return 'x86_64-elf-'+name if shutil.which('x86_64-elf-'+name) else name
class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    paths=[]
    def log_message(self,*_):pass
    def do_GET(self):
        self.paths.append(self.path)
        try:
            data=b'<title>Native task HTTP</title><h1>Tasks, audio and HTTP</h1><p>This real HTTP page arrived while two native C counters and two x87 context probes shared the desktop with MP3 playback.</p>'
            self.send_response(200);self.send_header('Content-Type','text/html');self.send_header('Content-Length',str(len(data)));self.end_headers()
            if self.path=='/slow':self.wfile.flush();time.sleep(1)
            self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass

def main():
    build=pathlib.Path(sys.argv[1]).resolve();d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-task-media-'));print(d,flush=True)
    # Four seconds of original mono material, encoded locally for the x87 decoder.
    with wave.open(str(d/'original.wav'),'wb') as out:
        out.setnchannels(1);out.setsampwidth(2);out.setframerate(44100)
        pcm=array.array('h',(round(11000*math.sin(2*math.pi*440*i/44100)) for i in range(4*44100)))
        out.writeframes(pcm.tobytes())
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(d/'original.wav'),'-codec:a','libmp3lame','-b:a','64k','-write_xing','0',str(d/'sample.mp3')],check=True)
    data=(d/'sample.mp3').read_bytes();(d/'media_fixture.h').write_text('static const unsigned char media_fixture[]={'+','.join(map(str,data))+'};\n')
    with (d/'task_media_examples.h').open('w') as out:
        for label,value in [('a',111),('b',222)]:
            subprocess.run(['nasm','-f','bin',f'-DVALUE={value}',str(ROOT/'tests/task_fpu.asm'),'-o',str(d/f'fpu-{label}.bex')],check=True)
            subprocess.run([sys.executable,str(ROOT/'tools/bin2c.py'),str(d/f'fpu-{label}.bex'),f'task_fpu_{label}'],stdout=out,check=True)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture);threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(ROOT),'-I',str(ROOT/'src'),'-I',str(build),'-I',str(d),f'-DHTTP_PORT="{server.server_port}"','-c',str(ROOT/'tests/task_media_guest.c'),'-o',str(d/'kernel.o')],check=True)
        objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
        subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'kernel.o'),*objects],check=True)
        subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
        c=constants();image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();install_kernel(image,(d/'kernel.bin').read_bytes(),c);(d/'disk.img').write_bytes(image)
        log=d/'serial.log';log.write_text('');capture=d/'capture.wav'
        with (d/'qemu.stderr').open('w') as errors:
            proc=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-drive',f'file={d/"disk.img"},format=raw,index=0,if=floppy','-netdev','user,id=net0','-device','rtl8139,netdev=net0','-audiodev',f'wav,id=test,path={capture},out.frequency=44100,out.channels=2,out.format=s16','-device','sb16,audiodev=test','-serial',f'file:{log}','-qmp','stdio','-display','none','-monitor','none','-no-reboot'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,bufsize=0)
            try:
                end=time.monotonic()+60
                while time.monotonic()<end:
                    text=log.read_text()
                    if 'TASK-MEDIA-HTTP-PASS' in text:
                        screenshot(proc,d/'task-media.ppm');break
                    if 'PANIC:' in text or proc.poll() is not None:raise AssertionError(text)
                    time.sleep(.1)
                else:raise AssertionError('combined task test timed out:\n'+log.read_text())
            finally:
                if proc.poll() is None:proc.terminate()
                proc.wait(timeout=5)
        with wave.open(str(capture),'rb') as wav:
            rate=wav.getframerate();pcm=array.array('h',wav.readframes(wav.getnframes()))
        assert len(pcm)>rate*6 and max(map(abs,pcm))>3000,'MP3 output missing or too short'
        assert '/slow' in Fixture.paths and '/index' in Fixture.paths,Fixture.paths
        print(log.read_text(),end='');print('Combined native task + x87/MP3 + HTTP + redraw pass. Screenshot:',d/'task-media.ppm')
    finally:server.shutdown();server.server_close()
if __name__=='__main__':main()
