"""Normal image decoding/viewer flows inside a disposable real-QEMU kernel.
Build first, then python3 tools/image_test.py build. Captures JPEG, PNG and alpha.
Never opens or modifies the user's saved boot image or data image.
"""
import argparse
import array
import json
import os
import pathlib
import re
import select
import shutil
import subprocess
import tempfile
import time
import wave
import zlib
from init_data import initialize
from layout import constants
from update_image import install_kernel

ROOT = pathlib.Path(__file__).resolve().parents[1]


def tool(name):
    cross = 'x86_64-elf-' + name
    return cross if shutil.which(cross) else name


class Qmp:
    def __init__(self, process):
        self.process = process
        self.buffer = bytearray()
        self.sequence = 0
        self.command('qmp_capabilities')

    def command(self, execute, arguments=None):
        self.sequence += 1
        request = dict(execute=execute, id=self.sequence)
        if arguments: request['arguments'] = arguments
        self.process.stdin.write(json.dumps(request).encode() + b'\n'); self.process.stdin.flush()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            while b'\n' in self.buffer:
                line, _, self.buffer = self.buffer.partition(b'\n')
                result = json.loads(line)
                if result.get('id') == self.sequence:
                    if 'error' in result: raise AssertionError(result)
                    return result
            ready, _, _ = select.select([self.process.stdout], [], [], max(0, deadline - time.monotonic()))
            if ready:
                data = os.read(self.process.stdout.fileno(), 65536)
                if not data: raise AssertionError('QEMU exited during QMP request')
                self.buffer.extend(data)
        raise AssertionError(f'QMP timeout for {execute}')


def verify_audio(path):
    with wave.open(str(path), 'rb') as recording:
        assert recording.getnchannels() == 2 and recording.getsampwidth() == 2
        rate = recording.getframerate()
        pcm = array.array('h', recording.readframes(recording.getnframes()))
    left, right = pcm[0::2], pcm[1::2]
    active = [i for i, (a, b) in enumerate(zip(left, right)) if a or b]
    assert active, 'image/audio capture is silent'
    first, end = active[0], active[-1] + 1
    assert end - first >= rate // 2, 'image/audio capture is too short'
    assert all(left[i] or right[i] for i in range(first, end)), 'silent gap during image work'
    if rate == 44100:
        for i in range(first, end):
            frame = i - first
            assert left[i] == (12000 if frame % 100 < 50 else -12000)
            assert right[i] == (9000 if frame % 50 < 25 else -9000)
    # QEMU's SB16 backend can quantize the requested 48 kHz to an effective 45 kHz.
    # Resampling makes edge samples nonintegral; uninterrupted sign transitions
    # must still be periodic, with at most one output-sample variation.
    for samples, limits in ((left, (49, 55)), (right, (24, 28))):
        changes = [i for i in range(first + 1, end) if (samples[i] > 0) != (samples[i - 1] > 0)]
        gaps = [b - a for a, b in zip(changes, changes[1:])]
        assert len(gaps) > 100 and limits[0] <= min(gaps) <= max(gaps) <= limits[1]
        assert max(gaps) - min(gaps) <= 1, 'audio phase discontinuity during image work'
    print(f'Continuous SB16 stereo verified across {(end - first) / rate:.2f}s of image work.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--maximum', action='store_true', help='use 1024 x 768 JPEG and PNG fixtures')
    parser.add_argument('--audio', action='store_true', help='measure image opens during real stereo SB16 playback')
    parser.add_argument('--audio-rate', type=int, choices=(44100, 48000), default=44100)
    parser.add_argument('--audio-format', choices=('wav', 'mp3'), default='wav')
    args = parser.parse_args(); build = args.build.resolve()
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-images-qemu-'))
    print(f'Image QEMU evidence: {directory}', flush=True)
    # The decoder must not require hosted helpers, x87, MMX, or SSE registers.
    undefined = subprocess.check_output([tool('nm'), '-u', str(build / 'image_decode.o')], text=True)
    assert not undefined.strip(), f'Unexpected decoder dependency: {undefined}'
    assembly = subprocess.check_output([tool('objdump'), '-d', '--no-show-raw-insn', str(build / 'image_decode.o')], text=True)
    mnemonics = re.findall(r'^\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)', assembly, re.M)
    assert not any(mnemonic.startswith('f') for mnemonic in mnemonics), 'Unexpected floating-point opcode'
    assert not re.search(r'%(?:xmm|mm)[0-9]', assembly), 'Unexpected SIMD register'
    fixtures = {'jpeg': 'landscape.jpg', 'png': 'landscape.png', 'alpha': 'transparent.png',
                'progressive': 'progressive.jpg', 'bmp': 'rgb.bmp', 'gif': 'animated.gif',
                'large_gif': 'large.gif', 'bos': 'paint.pbm'}
    if args.maximum:
        fixtures.update(jpeg='maximum.jpg', png='maximum.png')
    manifest = {item['name']: item for item in json.loads((ROOT / 'tests/fixtures/images/manifest.json').read_text())}
    header = ['typedef struct { unsigned short x, y; unsigned char rgba[4]; } ImagePixelCheck;\n',
              'typedef struct { const unsigned char *data; unsigned bytes, width, height, format, count; const ImagePixelCheck *checks; } ImageReference;\n']
    for name, filename in fixtures.items():
        data = (ROOT / 'tests/fixtures/images' / filename).read_bytes()
        header.append(f'static const unsigned char fixture_{name}[] = {{' + ','.join(str(b) for b in data) + '};\n')
        item = manifest[filename]; width, height = item['width'], item['height']
        rgba = zlib.decompress((ROOT / 'tests/fixtures/images' / item['reference']).read_bytes())
        pixels = []
        for i in range(24):
            x, y = (i * 71) % width, (i * 47) % height
            if item['format'] == 1: pixel = [rgba[y * width + x], 0, 0, 255]
            else: pixel = rgba[(y * width + x) * 4:(y * width + x) * 4 + 4]
            pixels.append('{' + f'{x},{y},' + '{' + ','.join(str(c) for c in pixel) + '}}')
        header.append(f'static const ImagePixelCheck checks_{name}[] = {{' + ','.join(pixels) + '};\n')
    header.append('static const ImageReference references[] = {\n')
    for name, filename in fixtures.items():
        item = manifest[filename]
        header.append('{' + f'fixture_{name}, sizeof fixture_{name}, {item["width"]}, {item["height"]}, {item["format"]}, 24, checks_{name}' + '},\n')
    header.append('};\n')
    mp3_reference = None
    if args.audio and args.audio_format == 'mp3':
        assert args.audio_rate == 44100, 'MP3 image/audio fixture uses 44.1 kHz'
        assert not args.maximum, 'Run maximum-size image tests with WAV; MP3 includes the large GIF fixture'
        encoded = directory / 'tone.mp3'
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                        'aevalsrc=0.3*sin(2*PI*330*t)|0.3*sin(2*PI*660*t):s=44100:d=8',
                        '-codec:a', 'libmp3lame', '-b:a', '64k', '-write_xing', '0', str(encoded)], check=True)
        header.append('static const unsigned char fixture_mp3[] = {' + ','.join(str(b) for b in encoded.read_bytes()) + '};\n')
        from mp3_test import host_decode
        mp3_reference = host_decode(encoded, directory)
    (directory / 'image_fixtures.h').write_text(''.join(header))
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32', '-fno-pie', '-fno-pic',
                    '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx', '-msoft-float',
                    '-I', str(ROOT / 'src'), '-I', str(directory),
                    *(['-DIMAGE_TEST_AUDIO', f'-DIMAGE_AUDIO_RATE={args.audio_rate}u'] if args.audio else []),
                    *(['-DIMAGE_TEST_MP3'] if args.audio and args.audio_format == 'mp3' else []),
                    '-c', str(ROOT / 'tests/image_guest.c'),
                    '-o', str(directory / 'guest.o')], check=True)
    subprocess.run(['nasm', '-f', 'elf', '-Dkmain=image_guest', '-p', str(build / 'layout.inc'),
                    str(ROOT / 'src/kernel_entry.asm'), '-o', str(directory / 'entry.o')], check=True)
    # Link production image/platform objects without the unrelated desktop apps.
    # Otherwise artwork fixtures compete with the whole desktop's stack budget.
    names = ('interrupts', 'process_entry', 'platform', 'bootinfo', 'process',
             'gfx', 'fs', 'persist', 'ata', 'rtc', 'image_decode', 'image_viewer',
             'audio')
    objects = [str(build / f'{name}.o') for name in names]
    objects.extend(str(path) for path in sorted(build.glob('media*.o')))
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386', '-z', 'noexecstack',
                    '-o', str(directory / 'kernel.elf'), str(directory / 'entry.o'), str(directory / 'guest.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'), str(directory / 'kernel.bin')], check=True)
    c = constants(); disk = bytearray(c['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes(); install_kernel(disk, (directory / 'kernel.bin').read_bytes(), c)
    image = directory / 'boot.img'; image.write_bytes(disk)
    data_image = directory / 'data.img'; initialize(data_image)
    log = directory / 'serial.log'
    with (directory / 'stderr.log').open('w') as errors:
        audio = ['-audiodev', f'wav,id=test,path={directory / "audio.wav"},out.frequency={args.audio_rate},out.channels=2,out.format=s16',
                 '-device', 'sb16,audiodev=test'] if args.audio else []
        process = subprocess.Popen(['qemu-system-i386', '-m', '64M', '-vga', 'std',
                                    '-drive', f'file={image},format=raw,index=0,if=floppy',
                                    '-drive', f'file={data_image},format=raw,index=0,if=ide',
                                    '-serial', f'file:{log}', '-qmp', 'stdio', '-display', 'none',
                                    '-monitor', 'none', '-no-reboot', '-nic', 'none', *audio],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0)
        try:
            qmp = Qmp(process); captured = set(); deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                text = log.read_text() if log.exists() else ''
                for label in ('JPEG', 'PNG', 'ALPHA'):
                    if f'IMAGE-QEMU-SHOT-{label}' in text and label not in captured:
                        qmp.command('screendump', {'filename': str(directory / f'{label.lower()}.ppm')})
                        captured.add(label)
                if 'IMAGE-QEMU-PASS' in text:
                    qmp.command('screendump', {'filename': str(directory / 'viewer.ppm')})
                    assert captured == {'JPEG', 'PNG', 'ALPHA'}
                    print(text, flush=True)
                    print('Verified screenshots:', ', '.join(str(p) for p in sorted(directory.glob('*.ppm'))), flush=True)
                    break
                if 'IMAGE-QEMU-FAIL' in text or 'PANIC' in text or process.poll() is not None:
                    break
                time.sleep(.05)
            else:
                raise AssertionError(f'Image guest timed out:\n{text}')
            if 'IMAGE-QEMU-PASS' not in text:
                raise AssertionError(f'Image guest failed:\n{text}\n{(directory / "stderr.log").read_text()}')
        finally:
            if process.poll() is None: process.terminate()
            process.wait(timeout=5)
    if args.audio:
        assert 'IMAGE-QEMU-AUDIO-PASS' in text
        if mp3_reference:
            from mp3_test import verify
            verify(directory / 'audio.wav', mp3_reference, 44100, 2)
        else:
            verify_audio(directory / 'audio.wav')


if __name__ == '__main__': main()
