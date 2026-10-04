"""Capture real MPEG-1/MP2 playback through QEMU SB16, with A/V controls.

Creates only disposable images and original media. Independently checked host
PCM/YUV is compared to actual guest output, including48k→44.1k resampling.
"""
import argparse
from array import array
import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
import wave
from mpeg_av_fixture import make_av_fixture,probe_av_fixture,decode_audio_reference
from video_fixture import decode_reference
from video_test import data_disk,tool
from layout import constants
from update_image import install_kernel
ROOT=pathlib.Path(__file__).resolve().parents[1]

def host_reference(source,directory,metadata):
    executable=directory/'mpeg-av-host'
    subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                   '-fsanitize=address,undefined','-DVIDEO_HOST_TEST','-DMEDIA_MP3_HOST_TEST','-I',str(ROOT/'src'),
                   str(ROOT/'tests/mpeg_av_host.c'),str(ROOT/'src/video.c'),str(ROOT/'src/media_mp3.c'),
                   '-lm','-o',str(executable)],check=True)
    independent=decode_audio_reference(source,metadata['source_channels'])
    count=independent.stat().st_size//4;actual=directory/'reference.s16'
    env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0';env['UBSAN_OPTIONS']='halt_on_error=1'
    subprocess.run([str(executable),str(source),str(actual),str(metadata['width']),str(metadata['height']),
                   '25','1',str(metadata['frames']),str(metadata['sample_rate']),str(metadata['source_channels']),
                   str(count),str(metadata['audio_lead_frames']),str(metadata['video_start_ms']),'0','0'],check=True,env=env)
    metadata['audio_frames']=count
    return actual

def build_guest(build,directory,metadata,controls,native=False):
    out=directory/('controls' if controls else 'playback');out.mkdir()
    if native:
        with (out/'mpeg_native_examples.h').open('w') as output:
            for label,value in [('a',111),('b',222)]:
                app=out/f'fpu-{label}.bex'
                subprocess.run(['nasm','-f','bin',f'-DVALUE={value}',str(ROOT/'tests/task_fpu.asm'),'-o',str(app)],check=True)
                subprocess.run(['python3',str(ROOT/'tools/bin2c.py'),str(app),f'mpeg_fpu_{label}'],stdout=output,check=True)
    values={'RATE':metadata['sample_rate'],'CHANNELS':metadata['source_channels'],
            'OUTPUT_RATE':44100 if metadata['sample_rate']>45000 else metadata['sample_rate'],
            'LEAD':metadata['audio_lead_frames'],'VIDEO_START':metadata['video_start_ms'],
            'FRAMES':metadata['frames'],'AUDIO_FRAMES':metadata['audio_frames']}
    (out/'mpeg_av_metadata.h').write_text(''.join(f'#define AV_{key} {value}u\n' for key,value in values.items()))
    subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin',
        '-mno-sse','-mno-mmx','-msoft-float',f'-DMPEG_AV_CONTROLS={int(controls)}',f'-DMPEG_NATIVE_TASKS={int(native)}','-I',str(ROOT),'-I',str(ROOT/'src'),
        '-I',str(build),'-I',str(out),'-c',str(ROOT/'tests/mpeg_av_guest.c'),'-o',str(out/'kernel.o')],check=True)
    objects=[str(p) for p in sorted(build.glob('*.o')) if p.name!='kernel.o']
    subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack',
                   '-o',str(out/'kernel.elf'),str(out/'kernel.o'),*objects],check=True)
    subprocess.run([tool('objcopy'),'-O','binary',str(out/'kernel.elf'),str(out/'kernel.bin')],check=True)
    c=constants();image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes()
    install_kernel(image,(out/'kernel.bin').read_bytes(),c);boot=out/'boot.img';boot.write_bytes(image)
    return out,boot,values['OUTPUT_RATE']

def run_guest(boot,data,directory,rate,controls):
    log=directory/'serial.log';log.write_text('');capture=directory/'capture.wav';seen=set()
    with (directory/'qemu.stderr').open('w') as errors:
        p=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-boot','a',
            '-drive',f'file={boot},format=raw,index=0,if=floppy','-drive',f'file={data},format=raw,index=0,if=ide',
            '-serial',f'file:{log}','-display','none','-monitor','none','-no-reboot',
            '-audiodev',f'wav,id=sound,path={capture},out.frequency={rate},out.channels=2,out.format=s16',
            '-device','sb16,audiodev=sound','-qmp','stdio'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors)
        json.loads(p.stdout.readline())
        def qmp(command,arguments=None):
            p.stdin.write(json.dumps({'execute':command,'arguments':arguments or {}}).encode()+b'\n');p.stdin.flush()
            while True:
                reply=json.loads(p.stdout.readline())
                if 'error' in reply:raise AssertionError(reply)
                if 'return' in reply:return reply['return']
        qmp('qmp_capabilities')
        try:
            deadline=time.monotonic()+80
            while time.monotonic()<deadline:
                text=log.read_text()
                for marker,name in [('MPEG-PAUSED-SCREEN','paused'),('MPEG-STOPPED-SCREEN','stopped'),('MPEG-FINISHED-SCREEN','finished')]:
                    if marker in text and marker not in seen:
                        qmp('screendump',{'filename':str(directory/(name+'.png')),'format':'png'});seen.add(marker)
                for marker,key in [('MPEG-WAIT-PAUSE','spc'),('MPEG-WAIT-RESUME','spc'),('MPEG-WAIT-STOP','s'),('MPEG-WAIT-REPLAY','ret')]:
                    if marker in text and marker not in seen:
                        qmp('send-key',{'keys':[{'type':'qcode','data':key}],'hold-time':50});seen.add(marker)
                if 'MPEG-AV-PASS' in text:time.sleep(.1);break
                if 'PANIC:' in text or p.poll() is not None:raise AssertionError(text)
                time.sleep(.02)
            else:raise AssertionError('MPEG A/V timeout:\n'+log.read_text())
        finally:p.terminate();p.wait(timeout=8)
    print(log.read_text(),end='')
    assert len(seen)==(7 if controls else 1),seen
    return capture

def resample(data,source_rate,output_rate):
    if source_rate==output_rate:return data
    result=array('h');frame=0;fraction=0;count=len(data)//2
    while frame<count:
        after=min(frame+1,count-1)
        for channel in range(2):
            value=data[frame*2+channel]*(output_rate-fraction)+data[after*2+channel]*fraction
            result.append(value//output_rate if value>=0 else -((-value)//output_rate))
        fraction+=source_rate;frame+=fraction//output_rate;fraction%=output_rate
    return result

def verify_capture(capture,reference,rate,output_rate):
    expected=resample(array('h',reference.read_bytes()),rate,output_rate)
    with wave.open(str(capture),'rb') as wav:
        assert wav.getnchannels()==2 and wav.getsampwidth()==2 and wav.getframerate()==output_rate
        actual=array('h',wav.readframes(wav.getnframes()))
    # The backend can prepend silence. Locate the first clearly audible frame,
    # then permit only a few samples of backend phase, never trim the source.
    e=next(i//2 for i in range(0,len(expected),2) if abs(expected[i])>1000)
    a=next(i//2 for i in range(0,len(actual),2) if abs(actual[i])>1000)
    best=None
    for shift in range(a-e-3,a-e+4):
        if shift<0 or shift*2+len(expected)>len(actual):continue
        errors=[actual[shift*2+i]-value for i,value in enumerate(expected)]
        rms=math.sqrt(sum(error*error for error in errors)/len(errors));peak=max(map(abs,errors))
        if best is None or rms<best[0]:best=(rms,peak,shift)
    assert best is not None and best[0]<=2 and best[1]<=12,best
    tail=actual[best[2]*2+len(expected):]
    assert all(abs(value)<=1 for value in tail), 'Unexpected sound after complete PCM tail'
    duration=len(expected)/2/output_rate
    assert abs(duration-(len(reference.read_bytes())/4/rate))<=1/output_rate
    print(f'Actual SB16: every {len(expected)//2} stereo PCM frame verified, RMS{best[0]:.4f}, peak{best[1]}, duration{duration:.6f}s ({rate}Hz source → {output_rate}Hz hardware).')
    if rate>45000:
        left=actual[(best[2]+output_rate//2)*2:(best[2]+output_rate//2+16384)*2:2]
        def tone_power(frequency):
            sine=sum(value*math.sin(2*math.pi*frequency*i/output_rate) for i,value in enumerate(left))
            cosine=sum(value*math.cos(2*math.pi*frequency*i/output_rate) for i,value in enumerate(left))
            return sine*sine+cosine*cosine
        peak_frequency=max(range(245,290),key=tone_power)
        assert peak_frequency==271,peak_frequency
        assert tone_power(271)>tone_power(271*45000/rate)*50
        print('Captured271Hz source tone remains271Hz; the uncorrected SB16 clamp pitch is excluded.')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=pathlib.Path)
    parser.add_argument('--rate',type=int,default=44100);parser.add_argument('--channels',type=int,choices=(1,2),default=2)
    parser.add_argument('--controls',action='store_true');parser.add_argument('--native',action='store_true');args=parser.parse_args()
    directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-mpeg-av-qemu-'));print(f'MPEG A/V evidence: {directory}',flush=True)
    source=make_av_fixture(directory,rate=args.rate,channels=args.channels,frames=75,width=320,height=240)
    metadata=probe_av_fixture(source);reference=host_reference(source,directory,metadata)
    (directory/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    disk=data_disk(directory,source);out,boot,output_rate=build_guest(args.build.resolve(),directory,metadata,args.controls,args.native)
    capture=run_guest(boot,disk,out,output_rate,args.controls)
    if args.native:assert 'MPEG-NATIVE-X87-PASS' in (out/'serial.log').read_text()
    if not args.controls:
        verify_capture(capture,reference,args.rate,output_rate)
        yuv=pathlib.Path(str(reference)+'.yuv').read_bytes();frame_size=metadata['width']*metadata['height']*3//2
        hashes=[]
        for n in range(metadata['frames']):
            hash=2166136261
            for byte in yuv[n*frame_size:(n+1)*frame_size]:hash=((hash^byte)*16777619)&0xffffffff
            hashes.append((n+1,hash))
        actual=[(int(n),int(hash,16)) for n,hash in re.findall(r'MPEG-FRAME (\d+) (0x[0-9A-F]+)',(out/'serial.log').read_text())]
        assert actual==hashes,'QEMU YUV frames changed during MP2 audio playback'
        print(f'Every {len(hashes)} simultaneous guest video frame matches the independent host path.')
    print('MPEG-1 + MP2 QEMU playback, audio clock, frame completion, x87 and desktop responsiveness verified.')
if __name__=='__main__':main()
