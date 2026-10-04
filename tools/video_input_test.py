"""Exercise MPEG Media Player through the unmodified production desktop.

Usage: python3 tools/video_input_test.py build
Requires QEMU, FFmpeg, binutils, and Pillow. Generates original MPEG-1/MP2,
WAV, and MP3 fixtures and uses only fresh disposable floppy/data images.
Never opens build/baseos.img or build/baseos-data.img. All guest actions use
real PS/2 keyboard/mouse input; bounded named-symbol pmemsave reads are
observations, not a test kernel, guest calls, or injected guest memory.
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import tempfile
import time
import wave
import zlib

from PIL import Image, ImageChops
from init_data import initialize
from mpeg_av_fixture import make_av_fixture, probe_av_fixture
from qemu_session import DesktopSession
from video_fixture import run_ffmpeg
from volume import DATA_LAYOUT, MAGIC, load, resolve

ROOT = pathlib.Path(__file__).resolve().parents[1]
STOPPED, LOADING, PLAYING, PAUSED = range(4)
WK_PLAYER = 21
VIDEO_FIELDS = ('state error width height fps_num fps_den aspect_num aspect_den '
                'total_frames displayed_frames audio_present arena_bytes late_resyncs '
                'presentation_skips audio_enabled audio_sample_rate audio_channels '
                'audio_total_frames audio_lead_frames video_start_ms audio_error').split()
AUDIO_FIELDS = ('state error available format sample_rate channels bits_per_sample '
                'output_rate total_frames played_frames volume underruns').split()
WIN_FIELDS = 'kind x y w h z open seq min maximized old_x old_y old_w old_h'.split()


class ProductionSession(DesktopSession):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.started = time.monotonic()
        self.key_events = []
        self.capture_events = []

    def key(self, key):
        # Use QMP's typed key command rather than parsing HMP sendkey output.
        # An 80 ms physical press remains short enough to avoid auto-repeat.
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 80})
        self.key_events.append(dict(key=key, hold_ms=80,
                                    elapsed_seconds=round(time.monotonic() - self.started, 3)))
        time.sleep(.12)

    def screenshot(self, name):
        # QEMU's synchronous PNG compression can stall its device/main loop.
        # Dump unchanged framebuffer pixels quickly, then encode PNG on the
        # host while the production guest continues to run normally.
        target = self.directory / name
        raw = target.with_suffix('.ppm')
        started = time.monotonic()
        self.command('screendump', {'filename': str(raw), 'format': 'ppm'})
        dumped = time.monotonic()
        with Image.open(raw) as pixels:
            pixels.save(target)
        self.capture_events.append(dict(name=name,
            elapsed_seconds=round(started - self.started, 3),
            qmp_dump_ms=round((dumped - started) * 1000, 3)))
        return target


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture_disk(directory, files):
    """Write a validated first snapshot to our newly created marked data disk."""
    disk = directory / 'data.img'
    assert initialize(disk), 'Expected a new disposable disk'
    data = bytearray(disk.read_bytes())
    records = [(0, -1, 1, '', b''), (1, 0, 1, 'Media', b'')]
    records += [(i + 2, 1, 0, path.name, path.read_bytes()) for i, path in enumerate(files)]
    payload = bytearray()
    for ident, parent, directory_flag, name, content in records:
        assert len(content) <= DATA_LAYOUT.file_limit
        payload += struct.pack('<HhBBHI24sI', ident, parent, directory_flag, 0, 0,
                               len(content), name.encode(), 0) + content
    assert len(payload) <= DATA_LAYOUT.payload_limit
    header = struct.pack('<6I', MAGIC, 4, len(records), len(payload), zlib.crc32(payload), 1)
    header += struct.pack('<I', zlib.crc32(header))
    offset = DATA_LAYOUT.lbas[0] * 512
    data[offset:offset + 512] = header.ljust(512, b'\0')
    data[offset + 512:offset + 512 + len(payload)] = payload
    nodes = load(data)[2]
    for path in files:
        assert nodes[resolve(nodes, '/Media/' + path.name)]['data'] == path.read_bytes()
    disk.write_bytes(data)
    return disk


class Observations:
    """Only named, size-checked data symbols in this production ELF are read."""
    def __init__(self, session, elf):
        self.session = session
        self.symbols = {}
        for line in subprocess.check_output(['nm', '-S', str(elf)], text=True).splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2].lower() in ('b', 'd'):
                address, size = int(parts[0], 16), int(parts[1], 16)
                self.symbols.setdefault(parts[3], []).append((address, size))
        self.video_address = self.status_address(elf, 'video_status', 84)
        self.audio_address = self.status_address(elf, 'audio_status', 48)

    def status_address(self, elf, function, size):
        disassembly = subprocess.check_output(
            ['objdump', '-d', '--disassemble=' + function, str(elf)], text=True)
        match = re.search(r'mov\s+\$0x([0-9a-f]+),%eax', disassembly)
        assert match, 'Cannot locate production ' + function + ' status pointer'
        address = int(match[1], 16)
        assert (address, size) in self.symbols['status'], 'Unexpected status symbol layout'
        return address

    def read(self, name, size=None):
        matches = self.symbols[name]
        assert len(matches) == 1, 'Ambiguous symbol: ' + name
        address, capacity = matches[0]
        size = capacity if size is None else size
        assert 0 < size <= capacity <= 4096, 'Unbounded observation: ' + name
        return self.session.memory(address, size)

    def integer(self, name):
        return struct.unpack('<i', self.read(name, 4))[0]

    def video(self):
        data = self.session.memory(self.video_address, 84)
        return dict(zip(VIDEO_FIELDS, struct.unpack('<21i', data)))

    def audio(self):
        data = self.session.memory(self.audio_address, 48)
        return dict(zip(AUDIO_FIELDS, struct.unpack('<12i', data)))

    def windows(self):
        data = self.read('wins')
        assert len(data) == 8 * 56, 'Unexpected production window layout'
        return [dict(zip(WIN_FIELDS, struct.unpack_from('<14i', data, i * 56)), slot=i)
                for i in range(8)]

    def player(self):
        return next((w for w in self.windows() if w['open'] and w['kind'] == WK_PLAYER), None)

    def snapshot(self):
        windows = self.windows()
        player = next((w for w in windows if w['open'] and w['kind'] == WK_PLAYER), None)
        front = max((w for w in windows if w['open'] and not w['min']),
                    key=lambda w: w['z'], default=None)
        return dict(video=self.video(), audio=self.audio(), player=player,
                    front_window=front,
                    launcher=self.integer('launcher_on'), menu=self.integer('open_menu'),
                    open_dialog=self.integer('open_dlg'),
                    modifiers={key: self.integer(key + '_down') for key in ('ctrl', 'alt', 'shift')},
                    audio_hardware_running=self.integer('hardware_running'),
                    audio_callback_installed=bool(self.integer('pcm_reader')),
                    position_100ms=self.integer('last_position'),
                    desktop_ticks=self.integer('frame_count'))


def move_mouse(session, observe, x, y):
    """Small relative PS/2 packets avoid overflow; observe the guest's cursor."""
    for _ in range(40):
        dx, dy = x - observe.integer('mouse_x'), y - observe.integer('mouse_y')
        if dx == dy == 0:
            return
        dx, dy = max(-80, min(80, dx)), max(-80, min(80, dy))
        session.command('human-monitor-command', {'command-line': f'mouse_move {dx} {dy}'})
        time.sleep(.035)
    raise AssertionError('PS/2 mouse did not reach taskbar target')


def restore_taskbar(session, observe):
    player = observe.player()
    assert player and player['min']
    count = observe.integer('tb_n')
    ids = struct.unpack('<8i', observe.read('tb_id'))
    index = ids[:count].index(player['slot'])
    xs = struct.unpack('<8i', observe.read('tb_x'))
    widths = struct.unpack('<8i', observe.read('tb_w'))
    move_mouse(session, observe, xs[index] + widths[index] // 2, observe.integer('fb_h') - 22)
    session.command('human-monitor-command', {'command-line': 'mouse_button 1'})
    time.sleep(.075)
    session.command('human-monitor-command', {'command-line': 'mouse_button 0'})
    session.wait(lambda: observe.player() is not None and not observe.player()['min'],
                 'Taskbar PS/2 click did not restore Media Player')


def viewport(player):
    # Mirror the public player client geometry, without calling into the guest.
    x, y, w, h = player['x'] + 1, player['y'] + 33, player['w'] - 2, player['h'] - 34
    viewport_h = max(68, h - 294)
    return (x + 16, y + 86, x + w - 16, y + 76 + viewport_h)


def intersection(a, b):
    result = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    assert result[2] - result[0] >= 8 and result[3] - result[1] >= 8, ('No useful overlap', a, b)
    return result


def image_changed(a, b, crop):
    with Image.open(a) as first, Image.open(b) as second:
        return ImageChops.difference(first.convert('RGB').crop(crop),
                                     second.convert('RGB').crop(crop)).getbbox() is not None


def healthy(snapshot):
    assert snapshot['video']['error'] == 0 and snapshot['audio']['error'] == 0, snapshot
    assert snapshot['audio']['underruns'] == 0, snapshot


def advancing(session, observe, seconds=.6):
    before = observe.snapshot()
    time.sleep(seconds)
    after = observe.snapshot()
    healthy(after)
    assert before['video']['state'] == after['video']['state'] == PLAYING, (before, after)
    assert after['video']['displayed_frames'] > before['video']['displayed_frames'], (before, after)
    assert after['audio']['played_frames'] > before['audio']['played_frames'], (before, after)
    assert after['position_100ms'] > before['position_100ms'], (before, after)
    return dict(before=before, after=after)


def wait_video(session, observe, message):
    started = time.monotonic()
    def ready():
        video, audio = observe.video(), observe.audio()
        assert video['error'] == audio['error'] == 0, (message, video, audio)
        return video['state'] == PLAYING and audio['state'] == PLAYING and audio['played_frames'] > 0
    # The normal 1.6 MiB program stream must finish bounded cooperative
    # loading promptly. This catches accidentally reverting to one tiny
    # packet per desktop tick, without requiring a particular batch size.
    session.wait(ready, message, seconds=15)
    return round(time.monotonic() - started, 3)


def audio_advancing(observe):
    before = observe.snapshot()
    time.sleep(.5)
    after = observe.snapshot()
    healthy(after)
    assert before['audio']['state'] == after['audio']['state'] == PLAYING, (before, after)
    assert after['audio']['played_frames'] > before['audio']['played_frames'], (before, after)
    assert after['video']['state'] == STOPPED and observe.integer('video_mode') == 0
    return dict(before=before, after=after)


def overlay_check(session, observe, label, crop, state_name, expected):
    session.wait(lambda: observe.integer(state_name) == expected, label + ' did not open')
    time.sleep(.12)
    crop = intersection(crop, viewport(observe.player()))
    first = session.screenshot(label + '-before.png')
    if label == 'open-dialog':
        # Retain row-level evidence for the case where an opaque modal hides
        # all normal video presentation/polling boundaries.
        time.sleep(.12)
        sample = session.screenshot(label + '-sample.png')
        with Image.open(first) as a, Image.open(sample) as b:
            difference = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
            rows = [y for y in range(a.height)
                    if difference.crop((0, y, a.width, y + 1)).getbbox()]
        session.capture_events[-1]['changed_rows_from_before'] = rows
        session.capture_events[-1]['changed_rows_at_64_boundaries'] = [y for y in rows if y % 64 == 0]
        bounds = viewport(observe.player())
        session.capture_events[-1]['changed_video_rows'] = [y for y in rows if bounds[1] <= y < bounds[3]]
        session.capture_events[-1]['state_at_sample'] = observe.snapshot()
    progress = advancing(session, observe, .8)
    second = session.screenshot(label + '-after.png')
    assert not image_changed(first, second, crop), label + ' was overwritten during playback'
    return dict(progress, stable_overlap=list(crop), screenshots=[str(first), str(second)])


def stopped(session, observe):
    session.wait(lambda: observe.video()['state'] == STOPPED and observe.audio()['state'] == STOPPED,
                 'Closing Media Player did not stop both transports')
    before = observe.snapshot()
    time.sleep(.6)
    after = observe.snapshot()
    assert after['video']['displayed_frames'] == before['video']['displayed_frames'] == 0
    assert after['audio']['played_frames'] == before['audio']['played_frames'] == 0
    assert after['audio_hardware_running'] == 0 and not after['audio_callback_installed']
    assert observe.read('frame') == bytes(36), 'Closing retained a decoded video frame'
    assert after['player'] is None, 'Closed player still has an open window'
    healthy(after)
    return dict(before=before, after=after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build.resolve()
    evidence = pathlib.Path(tempfile.mkdtemp(prefix='baseos-video-input-evidence-'))
    print('Production Media Player input evidence:', evidence, flush=True)
    identity = {name: sha(build / name) for name in ('boot.bin', 'kernel.bin', 'kernel.elf')}
    # Refuse stale symbols or a binary different from the ELF being observed.
    elf_binary = evidence / 'elf-kernel.bin'
    subprocess.run(['objcopy', '-O', 'binary', str(build / 'kernel.elf'), str(elf_binary)], check=True)
    assert sha(elf_binary) == identity['kernel.bin'], 'kernel.bin and kernel.elf differ'
    result = dict(passed=False, production_artifacts=identity,
                  build_info=json.loads((build / 'build_info.json').read_text()),
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  evidence_directory=str(evidence), checks={})
    video = make_av_fixture(evidence, name='ui-study', frames=375, fps='25', width=160, height=120)
    assert video.stat().st_size <= DATA_LAYOUT.file_limit
    result['fixture'] = dict(probe_av_fixture(video), bytes=video.stat().st_size, sha256=sha(video))
    wav, mp3 = evidence / 'ui-tone.wav', evidence / 'ui-tone.mp3'
    for output, codec in ((wav, 'pcm_s16le'), (mp3, 'libmp3lame')):
        run_ffmpeg(['-i', evidence / 'ui-study.source.wav', '-t', '4', '-c:a', codec, output])
    disk = fixture_disk(evidence, [video, mp3, wav])
    capture = evidence / 'sb16-output.wav'
    extra = ('-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback',
             '-audiodev', f'wav,id=sound,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
             '-device', 'sb16,audiodev=sound')
    checks = result['checks']
    try:
        with ProductionSession(build, 'video-input', extra=extra) as session:
            result['guest_directory'] = str(session.directory)
            print('Production guest:', session.directory, flush=True)
            observe = Observations(session, build / 'kernel.elf')
            try:
                session.boot()
                session.launch('Media Player')
                session.wait(lambda: observe.player() is not None, 'Launcher did not open Media Player')
                checks['launcher_app'] = observe.snapshot()
                session.screenshot('media-player-launcher.png')
                session.key('ctrl-w')
                session.launch(video.name)
                result['initial_load_seconds'] = wait_video(session, observe,
                    'MPEG file association did not start simultaneous video and MP2')
                first = session.screenshot('mpeg-playing-before.png')
                checks['file_association_and_progress'] = advancing(session, observe)
                second = session.screenshot('mpeg-playing-after.png')
                assert image_changed(first, second, viewport(observe.player())), 'Video framebuffer did not change'
                assert observe.video()['audio_enabled'] == 1 and observe.audio()['format'] == 3
                print('Production launcher/file association, visible video frames and MP2 audio progress passed.', flush=True)

                before_key = observe.snapshot()
                session.key('spc')
                session.wait(lambda: observe.video()['state'] == observe.audio()['state'] == PAUSED,
                             'Space did not pause both transports')
                time.sleep(.12)
                before = observe.snapshot()
                first = session.screenshot('mpeg-paused-before.png')
                time.sleep(.75)
                after = observe.snapshot()
                second = session.screenshot('mpeg-paused-after.png')
                assert before['video']['displayed_frames'] == after['video']['displayed_frames']
                assert before['audio']['played_frames'] == after['audio']['played_frames']
                assert before['position_100ms'] == after['position_100ms']
                assert not image_changed(first, second, viewport(observe.player()))
                checks['space_pause'] = dict(before_key=before_key, before=before, after=after)
                session.key('spc')
                session.wait(lambda: observe.video()['state'] == PLAYING, 'Space did not resume video')
                checks['space_resume'] = advancing(session, observe)
                session.screenshot('mpeg-resumed.png')

                # Maximize through PS/2 so the File menu overlaps the video
                # viewport even on a 1024-wide desktop. Compare opaque overlay
                # pixels while decoded frames and the audio clock advance.
                session.key('alt-ret')
                session.wait(lambda: observe.player()['maximized'] == 1, 'Player did not maximize')
                session.key('f10')
                menu_x = struct.unpack('<4i', observe.read('bar_x'))[1]
                checks['file_menu_overlay'] = overlay_check(session, observe, 'file-menu',
                    (menu_x + 6, 42, menu_x + 95, 220), 'open_menu', 1)
                session.key('esc')
                session.key('ctrl-spc')
                width, height = observe.integer('fb_w'), observe.integer('fb_h')
                checks['launcher_overlay'] = overlay_check(session, observe, 'launcher',
                    ((width - 520) // 2 + 20, 191, (width + 520) // 2 - 20, 410), 'launcher_on', 1)
                session.key('esc')
                session.key('ctrl-o')
                checks['open_dialog_overlay'] = overlay_check(session, observe, 'open-dialog',
                    ((width - 480) // 2 + 10, (height - 340) // 2 + 40,
                     (width + 480) // 2 - 10, (height + 340) // 2 - 60), 'open_dlg', 1)
                session.key('esc')
                print('Space pause/resume and menu, launcher, and modal overlay pixel preservation passed.', flush=True)

                session.key('ctrl-m')
                session.wait(lambda: observe.player()['min'] == 1, 'Ctrl+M did not minimize Media Player')
                checks['minimized_background_playback'] = advancing(session, observe, .8)
                session.screenshot('mpeg-minimized-playing.png')
                restore_taskbar(session, observe)
                checks['taskbar_restore'] = advancing(session, observe, .35)
                session.screenshot('mpeg-taskbar-restored.png')
                session.key('ctrl-w')
                checks['close_stops_audio_and_video'] = stopped(session, observe)
                session.screenshot('mpeg-closed.png')
                print('Minimized background playback, real PS/2 taskbar restore, and close stopping both transports passed.', flush=True)

                # Replay after closing, then choose adjacent alphabetically
                # sorted MP3/WAV files through the player's actual list keys.
                session.launch(video.name)
                wait_video(session, observe, 'MPEG did not reopen after close')
                session.key('s')
                session.wait(lambda: observe.video()['state'] == observe.audio()['state'] == STOPPED,
                             'S did not stop both MPEG transports')
                session.key('ret')
                wait_video(session, observe, 'Enter did not replay selected MPEG')
                checks['stop_and_replay'] = advancing(session, observe, .3)
                session.key('down')
                session.key('ret')
                session.wait(lambda: observe.audio()['state'] == PLAYING and observe.audio()['format'] == 2,
                             'Selecting MP3 did not start MP3 playback')
                assert observe.video()['state'] == STOPPED and observe.integer('video_mode') == 0
                checks['mp3_selection'] = audio_advancing(observe)
                session.screenshot('mp3-selected.png')
                session.key('down')
                session.key('ret')
                session.wait(lambda: observe.audio()['state'] == PLAYING and observe.audio()['format'] == 1,
                             'Selecting WAV did not start WAV playback')
                assert observe.video()['state'] == STOPPED and observe.integer('video_mode') == 0
                checks['wav_selection'] = audio_advancing(observe)
                session.screenshot('wav-selected.png')
                session.key('spc')
                session.wait(lambda: observe.audio()['state'] == PAUSED, 'WAV Space pause failed')
                session.key('spc')
                session.wait(lambda: observe.audio()['state'] == PLAYING, 'WAV Space resume failed')
                session.key('ctrl-w')
                checks['audio_only_close'] = stopped(session, observe)
                session.screenshot('all-media-closed.png')
                time.sleep(.75)
                assert 'PANIC:' not in session.log.read_text()
                print('MPEG stop/replay, MP3/WAV list selection, WAV pause/resume and audio-only close passed.', flush=True)
            except Exception:
                if session.process.poll() is None:
                    result['failure_state'] = observe.snapshot()
                    session.screenshot('failure.png')
                raise
        with wave.open(str(capture), 'rb') as audio:
            samples = audio.readframes(audio.getnframes())
            result['audio_capture'] = dict(path=str(capture), rate=audio.getframerate(),
                channels=audio.getnchannels(), frames=audio.getnframes(), sha256=sha(capture))
            assert any(samples), 'SB16 backend recorded only silence'
        assert all(sha(build / name) == digest for name, digest in identity.items()), 'Production build changed during test'
        result['screenshots'] = {p.name: sha(p) for p in session.directory.glob('*.png')}
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        if 'session' in locals():
            result['ps2_key_events'] = session.key_events
            result['framebuffer_captures'] = session.capture_events
        (evidence / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: normal production Media Player PS/2 controls and compositor overlays.', flush=True)
    print('Summary:', evidence / 'summary.json', flush=True)


if __name__ == '__main__':
    main()
