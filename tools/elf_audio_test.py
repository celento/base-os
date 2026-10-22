"""Audio streams of ELF programs (BOS_SYS_AUDIO_*) reach the SB16.

python3 tools/elf_audio_test.py build
Needs build/toolchain, mkfs.fat and mtools. Runs examples/elf/tone.c in the
tools/elf_runtime_test.py fixture with QEMU's SB16 recording to a WAV file,
then checks that the program was paced in real time and that the recording
holds the 441 Hz left and 882 Hz right square waves.
"""
import array
import math
import pathlib
import re
import subprocess
import sys
import tempfile

from elf_libc_test import TOOLCHAIN
from elf_runtime_test import ROOT, boot
from fat_volume_test import ENV


def power(samples, frequency, rate):
    n = len(samples)
    return math.hypot(sum(v * math.sin(2 * math.pi * frequency * i / rate) for i, v in enumerate(samples)),
                      sum(v * math.cos(2 * math.pi * frequency * i / rate) for i, v in enumerate(samples))) / n


def run(build):
    gcc = TOOLCHAIN / 'i686-baseos-gcc'
    if not gcc.exists():
        raise SystemExit('Build the toolchain first: python3 toolchain/build.py')
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-elf-audio-'))
    print('Work directory:', work, flush=True)
    program = work / 'tone'
    subprocess.run([str(gcc), '-O2', '-Wall', '-I', str(ROOT), str(ROOT / 'examples/elf/tone.c'),
                    '-o', str(program)], check=True)
    fat = work / 'fat.img'
    subprocess.run(['mkfs.fat', '-C', '-F', '32', str(fat), str(64 * 1024)], check=True, capture_output=True)
    subprocess.run(['mcopy', '-i', str(fat), str(program), '::TONE'], check=True, env=ENV)
    recording = work / 'capture.wav'
    text = boot(build, work, [('/disk1/TONE', 'tone', 0, 'tone ok: 88200 frames')],
                extra=('-boot', 'a', '-drive', f'file={fat},format=raw,if=ide,index=2',
                       '-audiodev', f'wav,id=test,path={recording},out.frequency=44100,out.channels=2,out.format=s16',
                       '-device', 'sb16,audiodev=test'))
    ms = int(re.search(r'tone ok: 88200 frames in (\d+) ms', text).group(1))
    assert 1500 <= ms <= 3500, f'not paced in real time: {ms} ms'
    # QEMU was stopped without finishing the WAV header: read the raw PCM.
    pcm = array.array('h', recording.read_bytes()[44:][:(len(recording.read_bytes()) - 44) // 4 * 4])
    left, right = pcm[0::2], pcm[1::2]
    loud = [i for i, v in enumerate(left) if abs(v) > 4000]
    assert loud and loud[-1] - loud[0] > 44100 * 3 // 2, 'less than 1.5 s of tone recorded'
    middle = (loud[0] + loud[-1]) // 2
    l, r = left[middle - 4096:middle + 4096], right[middle - 4096:middle + 4096]
    assert power(l, 441, 44100) > 4 * power(l, 882, 44100), 'left channel frequency'
    assert power(r, 882, 44100) > 4 * power(r, 441, 44100), 'right channel frequency'
    print(f'ELF_AUDIO_PASS: stream paced ({ms} ms for 2 s), {(loud[-1] - loud[0]) / 44100:.2f} s of stereo tone recorded.',
          flush=True)


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'build')
