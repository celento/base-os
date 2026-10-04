"""Compare DocStats batching during real native/desktop/SB16 work on disposable disks.

The baseline uses the same current C source and ABI with the previous per-4-KiB
cooperative yield / per-64-KiB redraw policy. Both variants analyze complete
2 MiB and 16 MiB inputs. No fault probes or modified saved user images.
"""
import argparse
import array
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import wave
from build_app import build as build_app, tool
from browser_test import screenshot
from init_data import initialize
from large_volume_test import node, place_snapshot, run as reboot
from layout import constants
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]


def prepare(build, directory):
    current = (ROOT / 'examples/c/docstats.c').read_text()
    old = current.replace('if(!(chunks%64u))draw((unsigned)expected);\n        else if(owner&&!(chunks%16u))bos_yield();',
                          'if(!(chunks%16u))draw((unsigned)expected);\n        if(owner)bos_yield();')
    assert current != old, 'DocStats scheduling changed; review the baseline variant'
    (directory / 'unbatched.c').write_text(old)
    build_app(directory / 'unbatched.c', directory / 'unbatched.bex')
    build_app(ROOT / 'examples/c/docstats.c', directory / 'docstats.bex')
    phrase = b'alpha beta\ngamma delta\n'
    large = (phrase * ((16777216 + len(phrase) - 1) // len(phrase)))[:16777216]
    inputs = {'/Documents/input-2m.txt': large[:2097152], '/Documents/input-16m.txt': large}
    nodes = {0: node(), 1: node('Documents', 0), 2: node('Programs', 0),
             3: node('input-2m.txt', 1, 0, data=inputs['/Documents/input-2m.txt']),
             4: node('input-16m.txt', 1, 0, data=large),
             5: node('docstats.bex', 2, 0, data=(directory / 'docstats.bex').read_bytes()),
             6: node('unbatched.bex', 2, 0, data=(directory / 'unbatched.bex').read_bytes())}
    disk = directory / 'data.img'; initialize(disk, profile='large')
    disk.write_bytes(place_snapshot(bytearray(disk.read_bytes()), nodes, volume.LARGE_DATA_LAYOUT))
    build_guest(build, directory)
    return inputs


def build_guest(build, directory):
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-ffreestanding', '-m32',
                    '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-msoft-float', '-I', str(ROOT), '-I', str(ROOT / 'src'),
                    '-I', str(build), '-c', str(ROOT / 'tests/docstats_workload_guest.c'),
                    '-o', str(directory / 'kernel.o')], check=True)
    objects = [str(p) for p in build.glob('*.o') if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                    '-z', 'noexecstack', '-o', str(directory / 'kernel.elf'), str(directory / 'kernel.o'), *objects], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(directory / 'kernel.elf'), str(directory / 'kernel.bin')], check=True)
    c = constants(); image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes(); install_kernel(image, (directory / 'kernel.bin').read_bytes(), c)
    (directory / 'boot.img').write_bytes(image)


def main(build, compile_only=False):
    directory = Path(tempfile.mkdtemp(prefix='baseos-docstats-workload-'))
    print(directory, flush=True); inputs = prepare(build, directory)
    if compile_only:
        print('Compiled variants, guest and large-profile disposable data; QEMU not started.'); return
    capture, log = directory / 'audio.wav', directory / 'serial.log'; log.write_text('')
    with (directory / 'qemu.stderr').open('w') as errors:
        proc = subprocess.Popen(['qemu-system-i386', '-m', '128M', '-vga', 'std', '-boot', 'a',
            '-drive', f'file={directory / "boot.img"},format=raw,index=0,if=floppy',
            '-drive', f'file={directory / "data.img"},format=raw,index=0,if=ide,cache=writeback',
            '-audiodev', f'wav,id=test,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=test', '-serial', f'file:{log}', '-qmp', 'stdio',
            '-display', 'none', '-monitor', 'none', '-no-reboot'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=errors, bufsize=0)
        try:
            deadline = time.monotonic() + 330
            while time.monotonic() < deadline:
                text = log.read_text()
                if 'DOCSTATS-WORKLOAD-PASS' in text:
                    screenshot(proc, directory / 'desktop.ppm'); break
                if 'PANIC:' in text or proc.poll() is not None:
                    raise AssertionError(text + errors.read_text())
                time.sleep(.1)
            else:
                raise AssertionError('DocStats workload deadline:\n' + log.read_text())
        finally:
            if proc.poll() is None: proc.terminate()
            proc.wait(timeout=10)
    verify_completed(directory, inputs)


def verify_completed(directory, inputs):
    log, capture = directory / 'serial.log', directory / 'audio.wav'
    assert 'DOCSTATS-WORKLOAD-PASS' in log.read_text()
    print(log.read_text(), end='')
    nodes = volume.load((directory / 'data.img').read_bytes())[2]
    checksums = {}
    for name, original in inputs.items():
        assert nodes[volume.resolve(nodes, name)]['data'] == original
        checksums[name] = hashlib.sha256(original).hexdigest()
        expected = f'Document: {name}\nBytes: {len(original)}\nWords: {len(original.split())}\nLines: {len(original.splitlines())}\nByte sum: {sum(original)}\n'.encode()
        suffix = '2m' if len(original) == 2097152 else '16m'
        for prefix in ('old', 'new'):
            assert nodes[volume.resolve(nodes, f'/Documents/{prefix}-{suffix}')]['data'] == expected
    with wave.open(str(capture), 'rb') as wav:
        pcm = array.array('h', wav.readframes(wav.getnframes()))
        peak = max(map(abs, pcm))
        assert peak > 3000 and wav.getnframes() > wav.getframerate()
        audio_check = {'frames': wav.getnframes(), 'sample_rate': wav.getframerate(),
                       'channels': wav.getnchannels(), 'peak': peak,
                       'comparison': 'Activity only; no sample-exact waveform or silent-gap comparison.'}
    reboot(directory / 'boot.img', directory / 'data.img', directory, 'reboot', 'DOCSTATS-WORKLOAD-REBOOT-PASS')
    after = volume.load((directory / 'data.img').read_bytes())[2]
    for name in ('old-2m', 'new-2m', 'old-16m', 'new-16m'):
        path = '/Documents/' + name
        assert after[volume.resolve(after, path)]['data'] == nodes[volume.resolve(nodes, path)]['data']
    metrics = {}
    for match in re.finditer(r'DOCSTATS-METRIC (\S+) ticks=(\d+) turns=(\d+) max_turn_ticks=(\d+) draws=(\d+) input_rounds=(\d+) audio_frames=(\d+) underruns=(\d+)', log.read_text()):
        label = match[1]
        metrics[label] = dict(zip(('ticks', 'turns', 'max_turn_ticks', 'draws', 'input_rounds', 'audio_frames', 'underruns'), map(int, match.groups()[1:])))
        metrics[label]['seconds'] = metrics[label]['ticks'] / 70
    assert len(metrics) == 4
    counter = int(nodes[volume.resolve(nodes, '/Documents/counter-2.txt')]['data'])
    assert counter >= 10 * sum(m['input_rounds'] for m in metrics.values())
    result = {'passed': True, 'metrics': metrics, 'input_sha256': checksums,
              'comparison': 'Same current source with previous per-4-KiB yield/per-64-KiB redraw policy versus new batching.',
              'screenshot': str(directory / 'desktop.ppm'), 'audio': str(capture), 'audio_validation': audio_check, 'peer_counter': counter,
              'checks': ['complete exact 2/16 MiB reports', 'startup document precedence', 'independent Counter progress/input',
                         'desktop redraw and idle Terminal input', 'real non-silent SB16 PCM with zero underruns', 'reboot persistence']}
    (directory / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path); parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args(); main(args.build.resolve(), args.compile_only)
