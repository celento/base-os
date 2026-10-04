"""Record real production QEMU framebuffer/SB16 playback of the Harbor example.

This is a short demonstration, not a frame-rate benchmark. It samples the real
framebuffer at eight images/second; no guest-memory writes or rendered mockups.
All disks and intermediate files are disposable. Requires FFmpeg and QEMU.
"""
import argparse
from array import array
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile
import time
import wave
from demo_data import install
from init_data import initialize
from layout import constants
from qemu_session import DesktopSession
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]


def record(build, output):
    if output.exists():
        raise ValueError('Choose a new recording filename')
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-recording-'))
    print('Recording evidence:', work, flush=True)
    c = constants()
    image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (build / 'kernel.bin').read_bytes(), c)
    boot = work / 'boot.img'; boot.write_bytes(image)
    data = work / 'data.img'; initialize(data)
    install(build, boot, data, int(time.time()))
    symbols = [line.split() for line in subprocess.check_output(['nm', '-S', str(build / 'kernel.elf')], text=True).splitlines()]
    address = next(int(p[0], 16) for p in symbols if len(p) == 4 and p[3] == 'status' and int(p[1], 16) == 48)
    captured = work / 'sb16.wav'
    extra = ['-drive', f'file={data},format=raw,index=0,if=ide', '-nic', 'none',
             '-audiodev', f'wav,id=out,path={captured},out.frequency=44100,out.channels=2,out.format=s16',
             '-device', 'sb16,audiodev=out']
    frames = []
    with DesktopSession(build, 'recording', image=boot, extra=extra) as guest:
        guest.boot()
        guest.launch('media player')
        for _ in range(5): guest.key('equal')
        guest.launch('harbor.mpg')
        def status(): return struct.unpack('<12I', guest.memory(address, 48))
        guest.wait(lambda: status()[0] == 2, 'Harbor playback did not start')
        time.sleep(.2)
        start_state = status()
        assert start_state[10] == 100 and start_state[4] == 44100
        began = time.monotonic()
        end_pause = None
        while time.monotonic() - began < 15:
            target = work / f'frame-{len(frames):04d}.ppm'
            stamp = time.monotonic() - began
            guest.command('screendump', {'filename': str(target), 'format': 'ppm'})
            frames.append((target, stamp))
            state = status()
            if state[0] == 5 or state[11]:
                raise AssertionError('Playback failed during recording: ' + str(state))
            if state[0] == 4:
                if end_pause is None: end_pause = stamp
                elif stamp - end_pause > .5: break
            time.sleep(max(0, began + len(frames) / 8 - time.monotonic()))
        assert state[0] == 4 and not state[11], state
        guest_evidence = str(guest.directory)
    # Locate the actual captured stream's leading backend silence, then cut at
    # the source-frame position read just before the first framebuffer sample.
    reference = work / 'source.s16'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i',
                    str(ROOT / 'assets/examples/harbor.mpg'), '-map', '0:a:0', '-vn',
                    '-ar', '44100', '-ac', '2', '-f', 's16le', str(reference)], check=True)
    expected = array('h', reference.read_bytes())
    with wave.open(str(captured), 'rb') as source:
        actual = array('h', source.readframes(source.getnframes()))
        assert source.getframerate() == 44100 and source.getnchannels() == 2
    e = next(i // 2 for i in range(0, len(expected), 2) if abs(expected[i]) > 1000)
    a = next(i // 2 for i in range(0, len(actual), 2) if abs(actual[i]) > 1000)
    offset = max(0, a - e + start_state[9])
    audio = work / 'recording.wav'
    with wave.open(str(audio), 'wb') as destination:
        destination.setnchannels(2); destination.setsampwidth(2); destination.setframerate(44100)
        destination.writeframes(actual[offset * 2:].tobytes())
    timeline = work / 'frames.txt'
    lines = []
    for index, (path, stamp) in enumerate(frames):
        duration = frames[index + 1][1] - stamp if index + 1 < len(frames) else .125
        lines += [f"file '{path}'", f'duration {duration:.6f}']
    lines.append(f"file '{frames[-1][0]}'")
    timeline.write_text('\n'.join(lines) + '\n')
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'concat', '-safe', '0',
                    '-i', str(timeline), '-i', str(audio), '-c:v', 'libx264', '-preset', 'veryfast',
                    '-crf', '22', '-pix_fmt', 'yuv420p', '-fps_mode', 'vfr', '-af', 'apad',
                    '-c:a', 'aac', '-b:a', '128k', '-shortest', '-movflags', '+faststart', str(output)], check=True)
    result = {'output': str(output), 'bytes': output.stat().st_size,
              'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
              'kernel_sha256': hashlib.sha256((build / 'kernel.bin').read_bytes()).hexdigest(),
              'guest_evidence': guest_evidence, 'framebuffer_samples': len(frames),
              'sample_rate_hz': 8, 'source_frame_at_start': start_state[9],
              'audio_underruns': state[11], 'sampling_seconds': frames[-1][1]}
    (work / 'recording.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    args = parser.parse_args()
    record(args.build.resolve(), args.output.resolve())
