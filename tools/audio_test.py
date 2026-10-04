"""Capture real guest SB16 output and validate its PCM; disposable disks only."""
import argparse
import array
import math
import pathlib
import shutil
import subprocess
import tempfile
import time
import wave
from layout import constants
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]

def tool(name):
    return 'x86_64-elf-' + name if shutil.which('x86_64-elf-' + name) else name

def capture(build, directory):
    subprocess.run([tool('gcc'), '-Os', '-ffreestanding', '-m32', '-fno-pie',
                    '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-msoft-float', '-I', str(ROOT), '-I', str(ROOT/'src'), '-I', str(build),
                    '-c', str(ROOT/'tests/audio_guest.c'), '-o', str(directory/'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build/'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(directory/'kernel.elf'),
                    str(directory/'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory/'kernel.elf'),
                    str(directory/'kernel.bin')], check=True)
    c = constants()
    disk = bytearray(c['DISK_SECTORS']*512)
    disk[:512] = (build/'boot.bin').read_bytes()
    install_kernel(disk, (directory/'kernel.bin').read_bytes(), c)
    image = directory/'disk.img'; image.write_bytes(disk)
    log = directory/'serial.log'; log.write_text('')
    recording = directory/'capture.wav'
    with (directory/'qemu.stderr').open('w') as errors:
        proc = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std',
            '-drive', f'file={image},format=raw,index=0,if=floppy',
            '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot',
            '-audiodev', f'wav,id=test,path={recording},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=test'], stderr=errors)
        try:
            deadline = time.monotonic()+45
            while time.monotonic()<deadline:
                text = log.read_text()
                if 'AUDIO-WAV-PASS' in text:
                    time.sleep(.5)
                    break
                if proc.poll() is not None or 'PANIC:' in text:
                    raise AssertionError(text)
                time.sleep(.1)
            else: raise AssertionError('Audio guest timed out:\n'+log.read_text())
        finally:
            proc.terminate();proc.wait(timeout=5)
    print(log.read_text(), end='')
    return recording

def power(samples, frequency, rate):
    # Direct projection around the target, avoiding optional numpy dependency.
    n=len(samples)
    return math.hypot(sum(v*math.sin(2*math.pi*frequency*i/rate) for i,v in enumerate(samples)),
                      sum(v*math.cos(2*math.pi*frequency*i/rate) for i,v in enumerate(samples)))/n

def check_recording(path):
    with wave.open(str(path),'rb') as wav:
        assert wav.getnchannels()==2 and wav.getsampwidth()==2
        rate=wav.getframerate();pcm=array.array('h',wav.readframes(wav.getnframes()))
    left=pcm[0::2];right=pcm[1::2]
    assert len(left)>rate*2, 'capture too short'
    assert max(map(abs,left))>2000, 'capture is silent'
    # Distinguish stereo segment from the earlier mono fixture by channel delta.
    stereo=[i for i,(l,r) in enumerate(zip(left,right)) if abs(l-r)>1500]
    assert len(stereo)>rate, 'stereo output missing or channels duplicated'
    # The complete 88,200-frame stereo waveform must survive every DMA wrap.
    start=stereo[0]-4
    assert len(left)-start>=88200, 'audio tail was truncated'
    for channel,values in enumerate((left,right)):
        period=50 if channel else 100
        for frame in range(88200):
            phase=frame%period
            expected=(-12000+phase*48000//period if phase<period//2 else
                      36000-phase*48000//period)
            assert values[start+frame]==expected, (channel,frame,values[start+frame],expected)
    center=stereo[len(stereo)//2]
    l=left[center-4096:center+4096];r=right[center-4096:center+4096]
    lp=power(l,441,rate);lr=power(l,882,rate)
    rp=power(r,882,rate);rl=power(r,441,rate)
    assert lp>2000 and rp>2000, (lp,rp)
    assert lp>lr*8 and rp>rl*8, 'stereo channel frequencies incorrect'
    print(f'Captured SB16 PCM: {len(left)/rate:.2f}s, stereo 441/882 Hz confirmed, peak {max(map(abs,left))}.')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=pathlib.Path)
    args=parser.parse_args()
    directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-audio-'))
    print(f'Audio evidence: {directory}',flush=True)
    check_recording(capture(args.build.resolve(),directory))

if __name__=='__main__':main()
