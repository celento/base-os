"""Create original MP3, decode in host/guest, compare actual SB16 output."""
import argparse
import array
import math
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import time
import wave
from audio_test import tool
from layout import constants
from update_image import install_kernel

ROOT=pathlib.Path(__file__).resolve().parents[1]

def make_fixture(directory,rate,channels,bitrate):
    original=directory/'original.wav'
    with wave.open(str(original),'wb') as output:
        output.setnchannels(channels);output.setsampwidth(2);output.setframerate(rate)
        pcm=array.array('h')
        for i in range(rate):
            for channel in range(channels):
                envelope=min(1,i/(rate*.03),(rate-i)/(rate*.03))
                pcm.append(round(math.sin(2*math.pi*(330+channel*330)*i/rate)*envelope*12000))
        output.writeframes(pcm.tobytes())
    encoded=directory/'sample.mp3'
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(original),
                    '-codec:a','libmp3lame','-b:a',bitrate,'-write_xing','1',str(encoded)],check=True)
    data=encoded.read_bytes();assert len(data)<16384
    header='static const unsigned char media_fixture[]={\n'
    header+=','.join(str(b) for b in data)+'\n};\n'
    (directory/'media_fixture.h').write_text(header)
    return encoded

def host_decode(encoded,directory):
    decoder=directory/'decode'
    cc=shutil.which('clang') or 'cc'
    subprocess.run([cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-DMEDIA_MP3_HOST_TEST','-I',str(ROOT/'src'),
        str(ROOT/'tests/media_mp3_host.c'),str(ROOT/'src/media_mp3.c'),'-o',str(decoder)],check=True)
    output=directory/'reference.s16'
    env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0'
    subprocess.run([str(decoder),str(encoded),str(output)],env=env,check=True)
    return output

def guest_capture(build,directory,fixture="mp3_guest.c",expected="AUDIO-MP3-PASS",screenshot_marker=None):
    subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie',
        '-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float',
        '-I',str(ROOT),'-I',str(ROOT/'src'),'-I',str(build),'-I',str(directory),
        '-c',str(ROOT/'tests'/fixture),'-o',str(directory/'kernel.o')],check=True)
    objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
    subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386',
        '-z','noexecstack','-o',str(directory/'kernel.elf'),str(directory/'kernel.o'),*objects],check=True)
    subprocess.run([tool('objcopy'),'-O','binary',str(directory/'kernel.elf'),str(directory/'kernel.bin')],check=True)
    c=constants();disk=bytearray(c['DISK_SECTORS']*512);disk[:512]=(build/'boot.bin').read_bytes()
    install_kernel(disk,(directory/'kernel.bin').read_bytes(),c)
    image=directory/'disk.img';image.write_bytes(disk)
    log=directory/'serial.log';log.write_text('');capture=directory/'capture.wav'
    with (directory/'qemu.stderr').open('w') as errors:
        proc=subprocess.Popen(['qemu-system-i386','-m','32M','-vga','std',
            '-drive',f'file={image},format=raw,index=0,if=floppy',
            '-serial',f'file:{log}','-display','none','-monitor','none','-no-reboot',
            '-audiodev',f'wav,id=test,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device','sb16,audiodev=test','-qmp','stdio'],stderr=errors,stdin=subprocess.PIPE,stdout=subprocess.PIPE)
        json.loads(proc.stdout.readline())
        def qmp(command,arguments=None):
            proc.stdin.write(json.dumps({'execute':command,'arguments':arguments or {}}).encode()+b'\n');proc.stdin.flush()
            while True:
                reply=json.loads(proc.stdout.readline())
                if 'error' in reply:raise AssertionError(reply)
                if 'return' in reply:return reply['return']
        qmp('qmp_capabilities')
        screenshot_done=False
        try:
            end=time.monotonic()+45
            while time.monotonic()<end:
                text=log.read_text()
                if screenshot_marker and screenshot_marker in text and not screenshot_done:
                    qmp('screendump',{'filename':str(directory/'player.png'),'format':'png'});screenshot_done=True
                if expected in text:time.sleep(.25);break
                if 'PANIC:' in text or proc.poll() is not None:raise AssertionError(text)
                time.sleep(.1)
            else:raise AssertionError('MP3 timeout:\n'+log.read_text())
        finally:proc.terminate();proc.wait(timeout=5)
    print(log.read_text(),end='')
    return capture

def verify(capture,reference,rate,channels):
    # Ask ffmpeg for the same output rate/channels, preserving minimp3's delay.
    normalized=reference.with_suffix('.stereo.s16')
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','s16le','-ar',str(rate),
        '-ac',str(channels),'-i',str(reference),*(['-af','pan=stereo|c0=c0|c1=c0'] if channels==1 else []),
        '-ar','44100','-ac','2','-f','s16le',str(normalized)],check=True)
    ref=array.array('h',normalized.read_bytes())
    with wave.open(str(capture),'rb') as wav:
        assert wav.getnchannels()==2 and wav.getframerate()==44100
        data=array.array('h',wav.readframes(wav.getnframes()))
    # Frame alignment uses the first clearly audible sample; the resamplers can
    # differ by one output frame, so check all offsets in a narrow neighborhood.
    r=next(i//2 for i in range(0,len(ref),2) if abs(ref[i])>1000)
    q=next(i//2 for i in range(0,len(data),2) if abs(data[i])>1000)
    best=None
    for shift in range(q-r-4,q-r+5):
        begin=max(0,-shift)*2;end=min(len(ref),len(data)-shift*2)
        errors=[data[i+shift*2]-ref[i] for i in range(begin,end)]
        rms=math.sqrt(sum(e*e for e in errors)/len(errors))
        if best is None or rms<best[0]:best=(rms,shift,len(errors))
    assert best[2]>rate*channels*.9 and best[0]<600,best
    print(f'MP3 SB16 capture matches decoded source: RMS error {best[0]:.2f}, {best[2]} interleaved samples.')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=pathlib.Path)
    parser.add_argument('--rate',type=int,default=22050);parser.add_argument('--channels',type=int,default=2)
    parser.add_argument('--bitrate',default='64k');args=parser.parse_args()
    directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-mp3-'));print(f'MP3 evidence: {directory}',flush=True)
    source=make_fixture(directory,args.rate,args.channels,args.bitrate)
    reference=host_decode(source,directory)
    verify(guest_capture(args.build.resolve(),directory),reference,args.rate,args.channels)
if __name__=='__main__':main()
