"""Real QEMU MPEG-1 playback, screenshots, physical keys, and frame verification.

Only disposable images are used. FFmpeg generates an original fixture and the
host decoder supplies bit-exact decoded-frame hashes; test_video.py separately
compares those frames to FFmpeg's independent decoder.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from layout import constants
from update_image import install_kernel
from init_data import initialize
from video_fixture import make_fixture
import volume

ROOT=pathlib.Path(__file__).resolve().parents[1]

def tool(name):
    cross='x86_64-elf-'+name
    return cross if shutil.which(cross) else name

def reference(source,directory):
    executable=directory/'video-host'
    subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-DVIDEO_HOST_TEST','-DMEDIA_MP3_HOST_TEST','-I',str(ROOT/'src'),
                    str(ROOT/'tests/video_host.c'),str(ROOT/'src/video.c'),str(ROOT/'src/media_mp3.c'),'-lm','-o',str(executable)],check=True)
    output=directory/'reference.yuv';env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0'
    env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
    subprocess.run([str(executable),str(source),str(output),'320','240','25','1','75','0'],env=env,check=True)
    data=output.read_bytes();size=320*240*3//2
    hashes=[]
    for n in range(75):
        hash=2166136261
        for byte in data[n*size:(n+1)*size]:hash=((hash^byte)*16777619)&0xffffffff
        hashes.append(hash)
    return hashes

def build_guest(build,directory):
    subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector',
                    '-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(ROOT),'-I',str(ROOT/'src'),
                    '-I',str(build),'-c',str(ROOT/'tests/video_guest.c'),'-o',str(directory/'kernel.o')],check=True)
    objects=[str(p) for p in sorted(build.glob('*.o')) if p.name!='kernel.o']
    subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack',
                    '-o',str(directory/'kernel.elf'),str(directory/'kernel.o'),*objects],check=True)
    subprocess.run([tool('objcopy'),'-O','binary',str(directory/'kernel.elf'),str(directory/'kernel.bin')],check=True)
    c=constants();image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes()
    install_kernel(image,(directory/'kernel.bin').read_bytes(),c)
    boot=directory/'boot.img';boot.write_bytes(image);return boot

def data_disk(directory,source):
    image=directory/'data.img';initialize(image);data=bytearray(image.read_bytes());c=constants()
    records=[(0,-1,1,'',b''),(1,0,1,'Video',b''),(2,1,0,'frame-study.mpg',source.read_bytes())]
    payload=bytearray()
    for id,parent,isdir,name,content in records:
        payload+=struct.pack('<HhBBHI24sI',id,parent,isdir,0,0,len(content),name.encode(),0)+content
    header=struct.pack('<6I',volume.MAGIC,4,len(records),len(payload),zlib.crc32(payload),1)
    header+=struct.pack('<I',zlib.crc32(header));start=c['DATA_FIRST_LBA']*512
    data[start:start+512]=header.ljust(512,b'\0');data[start+512:start+512+len(payload)]=payload
    assert volume.load(data)[2][2]['data']==source.read_bytes()
    image.write_bytes(data);return image

def run_guest(boot,data,directory,hashes):
    log=directory/'serial.log';log.write_text('');seen=set()
    with (directory/'qemu.stderr').open('w') as errors:
        process=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-boot','a',
            '-drive',f'file={boot},format=raw,index=0,if=floppy','-drive',f'file={data},format=raw,index=0,if=ide',
            '-serial',f'file:{log}','-display','none','-monitor','none','-no-reboot',
            '-audiodev','none,id=sound','-device','sb16,audiodev=sound','-qmp','stdio'],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors)
        json.loads(process.stdout.readline())
        def qmp(command,arguments=None):
            process.stdin.write(json.dumps({'execute':command,'arguments':arguments or {}}).encode()+b'\n');process.stdin.flush()
            while True:
                reply=json.loads(process.stdout.readline())
                if 'error' in reply:raise AssertionError(reply)
                if 'return' in reply:return reply['return']
        qmp('qmp_capabilities')
        try:
            deadline=time.monotonic()+75
            while time.monotonic()<deadline:
                text=log.read_text()
                # Capture stable states before sending the next transport key.
                for marker,name in [('VIDEO-PAUSED-SCREEN','paused'),('VIDEO-STOPPED-SCREEN','stopped'),('VIDEO-FINISHED-SCREEN','finished')]:
                    if marker in text and marker not in seen:
                        qmp('screendump',{'filename':str(directory/(name+'.png')),'format':'png'});seen.add(marker)
                for marker,key in [('VIDEO-WAIT-PAUSE','spc'),('VIDEO-WAIT-RESUME','spc'),('VIDEO-WAIT-STOP','s'),('VIDEO-WAIT-REPLAY','ret')]:
                    if marker in text and marker not in seen:
                        qmp('send-key',{'keys':[{'type':'qcode','data':key}],'hold-time':50});seen.add(marker)
                if 'VIDEO-QEMU-PASS' in text:break
                if 'PANIC:' in text or process.poll() is not None:raise AssertionError(text)
                time.sleep(.03)
            else:raise AssertionError('Video QEMU timeout:\n'+log.read_text())
        finally:
            process.terminate();process.wait(timeout=8)
    text=log.read_text();print(text,end='')
    actual=[(int(n),int(hash,16)) for n,hash in re.findall(r'VIDEO-FRAME (\d+) (0x[0-9A-F]+)',text)]
    assert actual==list(enumerate(hashes,1)), 'Guest frames differ from host decoder output'
    assert len(seen)==7, seen
    for name in ('paused','stopped','finished'):assert (directory/(name+'.png')).exists()
    print('All 75 guest Y/Cb/Cr frame hashes match. Real keyboard pause/resume/stop/replay, x87 preservation, responsive desktop, final frame and screenshots verified.')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=pathlib.Path)
    args=parser.parse_args();directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-video-qemu-'))
    print(f'Video QEMU evidence: {directory}',flush=True)
    source=make_fixture(directory);hashes=reference(source,directory)
    run_guest(build_guest(args.build.resolve(),directory),data_disk(directory,source),directory,hashes)
if __name__=='__main__':main()
