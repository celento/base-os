"""Production PS/2 verification of profile-owned WAV/MP3 source capacity.

Boots fresh default and large data disks with 128 MiB RAM, then compares every
captured PCM sample. The large workload uses an original Harbor-derived
three-minute >2 MiB MP3, ordinary filesystem compaction/deletion/copy/saves and
a protected native counter. No guest calls, memory writes or fault probes.
Requires the integrated post-fs_load_disk audio workspace configuration hook.
"""
import argparse
from array import array
import hashlib
import json
import math
import pathlib
import struct
import subprocess
import tempfile
import time
import wave

from docstats_input_test import TerminalCheck
from file_clipboard_input_test import FilesCheck
from init_data import initialize
from mp3_test import host_decode
from video_input_test import Observations
from volume import DATA_LAYOUT, LARGE_DATA_LAYOUT, encode_snapshot, load, resolve
from writer_input_test import WriterSession

ROOT = pathlib.Path(__file__).resolve().parents[1]


def make_disk(directory, profile, song):
    disk = directory / (profile + '.img')
    assert initialize(disk, profile=profile)
    layout = LARGE_DATA_LAYOUT if profile == 'large' else DATA_LAYOUT
    bulk = (b'alpha beta\n' * 190651)[:2097152] if profile == 'large' else b'original default data\n' * 1024
    padding = bytes(range(256)) * (8192 if profile == 'large' else 64)
    entries = [(0, -1, '', True, b''), (1, 0, 'Documents', True, b''),
               (2, 1, 'pad.bin', False, padding), (3, 1, song.name, False, song.read_bytes()),
               (4, 1, 'bulk.txt', False, bulk), (5, 0, 'Target', True, b'')]
    nodes = {i: dict(parent=parent, name=name, directory=int(folder), app=0,
                     data=content, modified=0) for i, parent, name, folder, content in entries}
    header, payload = encode_snapshot(nodes, layout, 1)
    raw = bytearray(disk.read_bytes()); start = layout.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    assert load(raw)[2] == nodes
    disk.write_bytes(raw)
    return disk, bulk


def verify_complete_capture(capture, reference):
    expected = array('h', reference.read_bytes())
    with wave.open(str(capture), 'rb') as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (2, 2, 44100)
        actual = array('h', source.readframes(source.getnframes()))
    # Locate backend-leading silence, then compare every source sample with no
    # trimming, gain fitting or omitted tail. Check nearby integer frame phases.
    expected_start = next(i // 2 for i in range(0, len(expected), 2) if abs(expected[i]) > 1000)
    actual_start = next(i // 2 for i in range(0, len(actual), 2) if abs(actual[i]) > 1000)
    best = None
    for shift in range(actual_start - expected_start - 3, actual_start - expected_start + 4):
        if shift < 0 or shift * 2 + len(expected) > len(actual):
            continue
        square = peak = 0
        for i, value in enumerate(expected):
            error = actual[shift * 2 + i] - value
            square += error * error
            peak = max(peak, abs(error))
        result = (math.sqrt(square / len(expected)), peak, shift)
        if best is None or result < best:
            best = result
    assert best is not None and best[0] <= 2 and best[1] <= 12, best
    assert all(abs(value) <= 1 for value in actual[best[2] * 2 + len(expected):]), 'Unexpected non-silent tail'
    result = dict(frames=len(expected) // 2, samples=len(expected), seconds=len(expected) / 88200,
                  rms=best[0], peak=best[1], leading_frames=best[2])
    print('Every captured source sample verified: ' + json.dumps(result), flush=True)
    return result


def node_exists(disk, path):
    try:
        resolve(load(disk.read_bytes())[2], path)
        return True
    except ValueError:
        return False


def run_profile(build, work, profile, song, reference):
    disk, bulk = make_disk(work, profile, song)
    capture = work / (profile + '-capture.wav')
    events = []
    with WriterSession(build, 'large-audio-' + profile, extra=[
            '-m', '128M', '-drive', f'file={disk},format=raw,index=0,if=ide',
            '-audiodev', f'wav,id=out,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=out']) as session:
        print('QEMU evidence: ' + str(session.directory), flush=True)
        session.boot(); observed = Observations(session, build / 'kernel.elf')
        expected_capacity = 16777216 if profile == 'large' else 2097152
        expected_base = 0x2000000 if profile == 'large' else 0x1700000
        assert observed.integer('source_capacity') == expected_capacity
        assert observed.integer('source_buffer') == expected_base
        assert observed.integer('data_backend') == (2 if profile == 'large' else 1)
        assert observed.integer('pool_base') == (0x3F00000 if profile == 'large' else 0x2000000)
        assert observed.integer('image_base') == (0x5F00000 if profile == 'large' else 0x2800000)
        assert load(disk.read_bytes())[2][4]['data'] == bulk
        session.launch('media player')
        for _ in range(5):
            session.key('equal')
        session.launch(song.name)
        session.wait(lambda: observed.audio()['state'] == 2, 'owned MP3 playback begins')
        assert observed.audio()['volume'] == 100
        start = time.monotonic()

        def event(name, finished=False):
            audio = observed.audio()
            assert audio['state'] == (4 if finished else 2), audio
            assert audio['error'] == 0 and audio['underruns'] == 0, audio
            assert observed.integer('source_buffer') == expected_base
            assert observed.integer('source_capacity') == expected_capacity
            events.append(dict(name=name, seconds=round(time.monotonic() - start, 3), **audio))
            (work / (profile + '-progress.json')).write_text(json.dumps(events, indent=2) + '\n')
            print(profile + ': ' + json.dumps(events[-1]), flush=True)

        event('128 MiB RAM selected only the mounted profile source arena')
        if profile == 'large':
            files = FilesCheck(session, build); terminal = TerminalCheck(session, build)
            session.launch('terminal')
            session.wait(lambda: files.front()['kind'] == 11, 'native Terminal launch is focused')
            native_slot = files.front()['slot']
            session.text('start /Programs/counter.bex'); session.key('ret')
            session.wait(lambda: terminal.terminal(native_slot)['canvas_on'] == 1, 'native counter is running')
            report_path = f'/Documents/counter-{native_slot + 1}.txt'
            event('protected native counter is running beside large audio')
            session.launch('files')
            session.wait(lambda: files.front()['kind'] == 4, 'Files launch is focused')
            files.folder(disk, '/Documents'); files.select(4); files.clipboard('ctrl-c')
            files.folder(disk, '/Target'); files.clipboard('ctrl-v')
            nodes = load(disk.read_bytes())[2]
            assert nodes[resolve(nodes, '/Target/bulk.txt')]['data'] == bulk
            event('2 MiB copy and snapshot save retain complete bytes')
            session.launch('terminal')
            session.wait(lambda: files.front()['kind'] == 11 and files.front()['slot'] != native_slot,
                         'second command Terminal launch is focused')
            command_slot = files.front()['slot']
            for path in ('/Documents/pad.bin', '/Documents/' + song.name):
                session.text('rm ' + path); session.key('ret')
                session.wait(lambda: not node_exists(disk, path), 'file deletion and compacting save complete', 60)
                event('compaction synchronized after deleting ' + path)
            assert load(disk.read_bytes())[2][4]['data'] == bulk
            session.key('ctrl-w')
            session.wait(lambda: not observed.windows()[command_slot]['open'],
                         'command Terminal close finishes before taskbar targeting')
            files.focus(native_slot)
            saves = []

            def save_counter():
                session.key('s')
                def saved():
                    if not node_exists(disk, report_path):
                        return False
                    nodes = load(disk.read_bytes())[2]
                    value = int(nodes[resolve(nodes, report_path)]['data'])
                    return not saves or value > saves[-1]
                session.wait(saved, 'native counter progress durably saved', 45)
                nodes = load(disk.read_bytes())[2]
                saves.append(int(nodes[resolve(nodes, report_path)]['data']))
                event('native counter save ' + str(saves[-1]))

            save_counter()
            screenshots = [str(session.screenshot('large-audio-files-native.png'))]
            for target in (90, 150):
                while time.monotonic() - start < target:
                    audio = observed.audio()
                    assert audio['state'] == 2 and not audio['error'] and not audio['underruns'], audio
                    time.sleep(.5)
                save_counter()
            assert saves[-1] > saves[0]
            session.wait(lambda: observed.audio()['state'] in (4, 5), 'entire three-minute MP3 finishes', 75)
            assert terminal.terminal(native_slot)['canvas_on'] == 1
            nodes = load(disk.read_bytes())[2]
            assert nodes[resolve(nodes, '/Target/bulk.txt')]['data'] == bulk
            assert nodes[resolve(nodes, '/Documents/bulk.txt')]['data'] == bulk
            assert not node_exists(disk, '/Documents/' + song.name)
        else:
            session.launch('terminal'); session.text('touch /Documents/kept.txt'); session.key('ret')
            session.wait(lambda: node_exists(disk, '/Documents/kept.txt'), 'default low filesystem save')
            event('default volume low arenas remain live and independent')
            session.wait(lambda: observed.audio()['state'] in (4, 5), 'default owned MP3 finishes', 30)
            screenshots = []
            assert load(disk.read_bytes())[2][4]['data'] == bulk
        event('entire source reached EOF with no underruns', finished=True)
        final = observed.audio(); assert final['played_frames'] == final['total_frames']
        time.sleep(.3)
        evidence = str(session.directory)
    comparison = verify_complete_capture(capture, reference)
    assert comparison['frames'] == final['total_frames']
    return dict(profile=profile, ram_mib=128, owned_capacity=expected_capacity, owned_base=hex(expected_base),
                mp3_bytes=song.stat().st_size, mp3_sha256=hashlib.sha256(song.read_bytes()).hexdigest(),
                pcm=comparison, qemu_evidence=evidence, events=events, screenshots=screenshots)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--profile', choices=('both', 'default', 'large'), default='both')
    args = parser.parse_args(); build = args.build.resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-large-audio-input-'))
    print('Large audio evidence: ' + str(work), flush=True)
    results = []
    profiles = ('default', 'large') if args.profile == 'both' else (args.profile,)
    for profile in profiles:
        folder = work / profile; folder.mkdir()
        song = folder / ('long.mp3' if profile == 'large' else 'short.mp3')
        if profile == 'large':
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-stream_loop', '-1',
                            '-i', str(ROOT / 'assets/examples/harbor.mp3'), '-t', '180',
                            '-codec:a', 'libmp3lame', '-b:a', '192k', '-write_xing', '0',
                            '-id3v2_version', '0', str(song)], check=True)
            assert 2097152 < song.stat().st_size <= 16777216
        else:
            song.write_bytes((ROOT / 'assets/examples/harbor.mp3').read_bytes())
        reference = host_decode(song, folder)
        results.append(run_profile(build, folder, profile, song, reference))
        (work / 'results.json').write_text(json.dumps(dict(
            passed=len(results) == len(profiles), requested_profiles=profiles, results=results), indent=2) + '\n')
    print('Complete profile-owned audio verification passed: ' + str(work / 'results.json'), flush=True)


if __name__ == '__main__':
    main()
