"""Held-input production Pointer + peer + MP3 + IDE-save gate.

Preparation is host-only. Guest execution requires --run and an explicit
exclusive-slot acknowledgement. The unchanged hour14 production boot image,
real Pointer-window, and unchanged public-SDK snapshot peer are used together.
Only PS/2, source-decoded screenshots, normal serial and stopped disks are read.
This collector has no runtime, SDK, example or guest-algorithm modifications.
"""
import argparse
from array import array
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time
import traceback
import wave

import numpy as np
from PIL import Image
from build_app import build as build_app
from large_audio_input_test import verify_complete_capture
from mp3_test import host_decode
from native_pointer_input_test import Session as InputSession
from native_window_input_test import (CORE_ARTIFACTS, NativeFont, PROFILES,
    ShellFont, arranged_window, file_record, point, reconstruct, native_viewport,
    require, save_json, verify_build_directory)
from snapshot_responsiveness_test import (Session as ProbeDecoder, fixture,
    outstanding_snapshots, snapshot_jobs, validate_saved, MAGIC)
from volume import data_layout, encode_snapshot, load, resolve

ROOT = Path(__file__).resolve().parents[1]
REVISION = '33870aeacf4629007713c13afed98be43fe523ac'
IMMUTABLE_SOURCE_PATHS = ('src', 'sdk', 'examples', 'assets', 'third_party', 'Makefile',
                          'tests/snapshot_responsive_app.c', 'tests/media_mp3_host.c')
AUDIO_SECONDS = 45
SAVE_TIMEOUT = 180
LANES = ('production-boot', 'peer-live-input', 'owned-pointer-draw-publication',
         'mp3-complete-sb16', 'save-overlap', 'stopped-disk', 'reboot-full-payload')


def verify_records(records):
    for name, expected in records.items():
        actual = file_record(expected['path'])
        require(actual['bytes'] == expected['bytes'] and actual['sha256'] == expected['sha256'],
                'Held input changed: ' + name)


def verify_inputs(manifest):
    for group in ('build', 'source', 'apps', 'media', 'fixtures'):
        verify_records(manifest[group])
    require(manifest['source_revision'] == REVISION, 'Wrong frozen source revision')
    require(manifest['build_info']['revision'] == REVISION and not manifest['build_info']['dirty'],
            'Build is not the clean hour14 source')


def prepare(build, output, build_log):
    output.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema=1, status='PREPARING; NO GUEST RUN', preparation_only=True,
        runner_guest_qualified=False, source_revision=REVISION, profiles={},
        lanes={lane: 'NOT RUN' for lane in LANES}, build={}, source={}, apps={}, media={}, fixtures={},
        observation_policy='Ordinary PS/2; exact source-decoded visible pixels; normal serial; stopped-disk only',
        excluded=['guest-memory inspection', 'debugger', 'live-volume reads', 'fixture kernel',
                  'intentional faults', 'fuzzing', 'security reproduction', 'kernel/runtime/SDK/example edits'],
        limits={'save_timeout_seconds': SAVE_TIMEOUT, 'audio_seconds': AUDIO_SECONDS,
                'key_echo_timeout_seconds': 10, 'reboot_read_timeout_seconds': 180},
        claims_not_made=['latency SLA', 'unobserved underrun counter', 'WAV overlap',
                         'HTTP/network coverage', 'hardware beyond this QEMU SB16 profile'])
    save_json(output/'manifest.json', manifest)
    try:
        changed = subprocess.check_output(['git', 'diff', REVISION, '--', *IMMUTABLE_SOURCE_PATHS], cwd=ROOT)
        require(not changed, 'Runtime, SDK, original assets and examples must remain frozen')
        manifest['collector_worktree_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT))
        manifest['collector_revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        manifest['build_directory'] = str(build)
        for name in (*CORE_ARTIFACTS, 'build_info.json', 'pointer-window.bex', 'chime.wav'):
            manifest['build'][name] = file_record(build/name)
        manifest['build']['hour14-build.log'] = file_record(build_log)
        manifest['build_info'] = json.loads((build/'build_info.json').read_text())
        manifest['boot_consistency'] = verify_build_directory(build)
        paths = subprocess.check_output(['git', 'ls-files', 'src', 'sdk', 'assets', 'examples', 'tools',
                                         'third_party', 'Makefile', 'tests/snapshot_responsive_app.c', 'tests/media_mp3_host.c'], cwd=ROOT, text=True).splitlines()
        paths += ['tools/native_gui_media_input_test.py', 'tests/test_native_gui_media_collector.py']
        for name in sorted(set(paths)):
            manifest['source'][name] = file_record(ROOT/name)
        shutil.copyfile(build/'pointer-window.bex', output/'pointer-window.bex')
        build_app(ROOT/'tests/snapshot_responsive_app.c', output/'snap-probe.bex')
        for name in ('pointer-window.bex', 'snap-probe.bex'):
            manifest['apps'][name] = file_record(output/name)
        require((output/'pointer-window.bex').read_bytes()[:4] == b'BEX2', 'Expected real native-window app')
        require(struct.unpack_from('<I', (output/'pointer-window.bex').read_bytes(), 12)[0] == 1,
                'Pointer must explicitly require a native-v1 window')
        song = output/'harbor-test.mp3'
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-stream_loop', '-1',
            '-i', str(ROOT/'assets/examples/harbor.mp3'), '-t', str(AUDIO_SECONDS),
            '-ar', '44100', '-ac', '2', '-codec:a', 'libmp3lame', '-b:a', '96k', '-write_xing', '0', '-id3v2_version', '0', str(song)]
        subprocess.run(command, check=True)
        manifest['audio_encoding_command'] = command
        stream_info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
            'stream=codec_name,sample_rate,channels', '-of', 'json', str(song)], text=True))
        require(len(stream_info['streams']) == 1 and stream_info['streams'][0] ==
                {'codec_name': 'mp3', 'sample_rate': '44100', 'channels': 2},
                'Oracle requires exactly stereo 44100 Hz MP3')
        manifest['encoded_audio_stream'] = stream_info
        reference = host_decode(song, output)
        # Keep the exact original production WAV for future distinct coverage;
        # its presence is never reported as a guest playback pass.
        shutil.copyfile(build/'chime.wav', output/'original-chime.wav')
        for name, path in [('original-harbor.mp3', ROOT/'assets/examples/harbor.mp3'),
                           ('harbor-test.mp3', song), ('reference.s16', reference),
                           ('original-chime.wav', output/'original-chime.wav'), ('host-decoder', output/'decode')]:
            manifest['media'][name] = file_record(path)
        require(reference.stat().st_size % 4 == 0, 'Reference must be stereo signed16 PCM')
        manifest['audio_reference_frames'] = reference.stat().st_size//4
        x87_decoder, x87_reference = output/'decode-x87', output/'reference-x87.s16'
        calibration_command = ['cc', '-std=gnu11', '-Os', '-g', '-Wall', '-Wextra', '-Werror',
            '-mfpmath=387', '-DMEDIA_MP3_HOST_TEST', '-I'+str(ROOT/'src'),
            str(ROOT/'tests/media_mp3_host.c'), str(ROOT/'src/media_mp3.c'), '-o', str(x87_decoder)]
        subprocess.run(calibration_command, check=True)
        subprocess.run([str(x87_decoder), str(song), str(x87_reference)], check=True)
        baseline = np.frombuffer(reference.read_bytes(), dtype='<i2').astype(np.int32)
        alternate = np.frombuffer(x87_reference.read_bytes(), dtype='<i2').astype(np.int32)
        require(baseline.shape == alternate.shape, 'Host decoder extents disagree')
        delta = alternate-baseline
        calibration = dict(samples=int(delta.size), rms=float(np.sqrt(np.mean(delta.astype(np.float64)**2))),
            peak=int(np.max(np.abs(delta))), differing_samples=int(np.count_nonzero(delta)),
            scope='Host x87 versus existing host oracle only; not an i386 production guest qualification',
            command=calibration_command)
        require(calibration['rms'] <= 2 and calibration['peak'] <= 12, 'Host x87 rounding exceeds frozen oracle')
        save_json(output/'host-rounding-calibration.json', calibration)
        manifest['host_rounding_calibration'] = calibration
        for path in (x87_decoder, x87_reference, output/'host-rounding-calibration.json'):
            manifest['media'][path.name] = file_record(path)
        manifest['host_tools'] = {name: subprocess.check_output([name, '-version' if name in ('ffmpeg', 'ffprobe') else '--version'], text=True).splitlines()[0]
                                  for name in ('ffmpeg', 'ffprobe', 'gcc', 'ld', 'objcopy')}
        manifest['host_compiler'] = shutil.which('clang') or shutil.which('cc')
        manifest['host_decoder_executable'] = file_record(output/'decode')
        manifest['audio_oracle'] = dict(decoder='Frozen src/media_mp3.c and third_party/minimp3/minimp3.h, MINIMP3_NO_SIMD',
            host='Existing tests/media_mp3_host.c; MEDIA_MP3_HOST_TEST; native compiler float rounding',
            guest='Unchanged production i386 x87 decoder; Makefile -mhard-float -mfpmath=387',
            sample_rate=44100, channels=2, sample_format='signed 16-bit little-endian interleaved stereo',
            resampling='None: source 44100 Hz equals production output_rate and QEMU WAV backend 44100 Hz',
            volume='Ordinary player + keys select 100%; full-scale SB16 mixer',
            alignment='First left-channel sample with abs(sample)>1000; test fixed integer-frame offsets -3..+3',
            source_extent='Every decoded source sample, including decoder delay and complete tail; no trimming or gain fit',
            tolerance={'maximum_rms_sample_error': 2, 'maximum_peak_sample_error': 12,
                       'maximum_absolute_post_source_tail_sample': 1,
                       'maximum_absolute_backend_leading_sample': 1},
            note='This shared-decoder oracle tests production delivery fidelity, not independent MP3 codec conformance')
        for profile, ram in PROFILES.items():
            folder = output/profile; folder.mkdir()
            disk, nodes, details = fixture(folder, profile, song, output/'snap-probe.bex')
            ident = next(i for i in range(data_layout(profile).node_limit) if i not in nodes)
            nodes[ident] = dict(parent=resolve(nodes, '/Programs'), name='pointer-window.bex', directory=0,
                               app=0, data=(output/'pointer-window.bex').read_bytes(), modified=123400+ident)
            header, payload = encode_snapshot(nodes, data_layout(profile), 1)
            raw = bytearray(disk.read_bytes()); offset = data_layout(profile).lbas[0]*512
            raw[offset:offset+512] = header.ljust(512, b'\0')
            raw[offset+512:offset+512+len(payload)] = payload
            disk.write_bytes(raw)
            require(load(raw)[2] == nodes, 'Fresh valid fixture round-trip differs')
            manifest['fixtures'][profile] = file_record(disk)
            details['initial_snapshot_bytes'] = len(payload)
            manifest['profiles'][profile] = dict(ram_mib=ram, status='NOT RUN', **details, free_payload_bytes=data_layout(profile).payload_limit-len(payload),
                initial_files={str(i): {'name': n['name'], 'bytes': len(n['data']),
                    'sha256': hashlib.sha256(n['data']).hexdigest()} for i,n in nodes.items()})
        verify_inputs(manifest)
        manifest['status'] = 'PREPARED; NO GUEST RUN'
    except BaseException as error:
        manifest['status'] = 'PREPARATION FAILED'; manifest['failure'] = repr(error)
        raise
    finally:
        save_json(output/'manifest.json', manifest)
    return manifest


def verify_audio(capture, reference):
    result = verify_complete_capture(capture, reference)
    with wave.open(str(capture), 'rb') as source:
        leading = array('h', source.readframes(result['leading_frames']))
    require(all(abs(value) <= 1 for value in leading), 'Non-silent backend prefix')
    result['leading_silence_verified'] = True
    return result


class Session(InputSession):
    """Only four QMP operations; inherited memory API always raises."""
    def __init__(self, *args, **kwargs):
        self.events = []; self.canvas = None; self.last_input = time.monotonic()
        try:
            super().__init__(*args, **kwargs)
        except BaseException as error:
            # DesktopSession may fail after spawning but before this instance
            # reaches the caller's context manager. Keep and stop that guest.
            try:
                if hasattr(self, 'process'): self.close()
                elif hasattr(self, 'stderr'): self.stderr.close()
            finally:
                if hasattr(self, 'directory'):
                    save_json(self.directory/'startup-failure.json', dict(error=repr(error),
                        input_events=self.events, process_stopped=not hasattr(self, 'process') or self.process.poll() is not None))
            raise RuntimeError('QEMU startup failed; retained evidence: '+str(getattr(self, 'directory', 'not allocated'))) from error

    def command(self, name, arguments=None):
        require(name in ('qmp_capabilities', 'send-key', 'input-send-event', 'screendump'),
                'Disallowed observation/control operation')
        began = time.monotonic()
        if name in ('send-key', 'input-send-event'): self.last_input = began
        result = super().command(name, arguments)
        self.events.append(dict(command=name, arguments=arguments, wall=began,
                                completed_wall=time.monotonic()))
        return result

    def frame(self, keep=None):
        path = self.directory/((keep or 'latest')+'.png')
        self.command('screendump', {'filename': str(path), 'format': 'png'})
        return np.array(Image.open(path).convert('RGB')), time.monotonic()

    observe = ProbeDecoder.observe

    def keep_awake(self):
        if time.monotonic()-self.last_input > 10: self.key('shift')

    def observe_until(self, predicate, message, seconds=90, keep=None):
        deadline = time.monotonic()+seconds
        while time.monotonic() < deadline:
            self.keep_awake()
            state, pixels, wall = self.observe()
            if state is not None and predicate(state):
                return self.observe(keep) if keep else (state, pixels, wall)
            require(self.process.poll() is None and 'PANIC:' not in self.log.read_text(), message)
            time.sleep(.04)
        raise AssertionError(message)

    def idle_saves(self, label):
        stable = None
        def done():
            nonlocal stable
            self.keep_awake()
            log = self.log.read_text()
            require('result=error' not in log and 'FS save failed' not in log, 'Production save failed')
            if outstanding_snapshots(log): stable=None; return False
            if stable is None: stable=time.monotonic()
            return time.monotonic()-stable >= 2
        self.wait(done, label, SAVE_TIMEOUT)

    def normal_shutdown(self):
        for key in ('f10', 'right', 'right', 'down', 'down', 'down'): self.key(key)
        self.frame('shutdown-menu'); self.key('ret')
        require(self.process.wait(timeout=SAVE_TIMEOUT) == 0, 'Guest shutdown did not complete')

    def __exit__(self, kind, error, tb):
        if error is not None:
            try: self.frame('failure')
            except Exception: pass
        save_json(self.directory/'input-events.json', self.events)
        return super().__exit__(kind, error, tb)


def read_pair(session, font, name):
    probe, pixels, wall = session.observe(name)
    require(probe is not None, 'Peer canvas is missing')
    pointer = font.pointer(pixels, arranged_window(pixels, 'right'))
    return dict(probe=probe, pointer=pointer, wall=wall, screenshot=str(session.directory/(name+'.png')))


def player_state(session, font, expected, name):
    pixels, wall = session.frame(name)
    window = arranged_window(pixels)
    # Native production player_draw(wx+1, wy+TITLE_H+1, ww-2,...).
    x,y,w,h = window; client_x,client_y,client_w = x+1,y+33,w-2
    text_width = sum(font.advance[ord(c)-32] for c in expected)
    require(font.matches(pixels, client_x+client_w-30-text_width, client_y+56, expected, 'ui'),
            'Media Player does not visibly say '+expected)
    return dict(state=expected, wall=wall, screenshot=str(session.directory/(name+'.png')))


def copy_stopped_session(session, folder):
    require(session.process.poll() is not None, 'Session must stop before evidence copy')
    target = folder/session.directory.name
    shutil.copytree(session.directory, target)
    return str(target)


def run_profile(manifest, prepared, folder, profile):
    verify_inputs(manifest)
    folder.mkdir()
    disk = folder/'data.img'; shutil.copyfile(manifest['fixtures'][profile]['path'], disk)
    original = load(disk.read_bytes())[2]
    boot = folder/'production-boot.img'; shutil.copyfile(manifest['build']['baseos.img']['path'], boot)
    capture = folder/'capture.wav'
    record = dict(profile=profile, ram_mib=PROFILES[profile], passed=False, lanes={x:'NOT RUN' for x in LANES},
                  samples=[], input_to_visible_upper_bounds_ms=[], normal_shutdown=False)
    save_json(folder/'results.json', record)
    font, shell = NativeFont(), ShellFont()
    session = None; reboot = None
    try:
        with Session(manifest['build_directory'], 'gui-media-'+profile, image=boot, extra=(
            '-m', str(PROFILES[profile])+'M', '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback',
            '-audiodev', f'wav,id=out,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=out')) as session:
            record['qemu_directory'] = str(session.directory)
            session.boot(); session.origin(); session.move(1275,670)
            require('Build '+REVISION[:12] in session.log.read_text(), 'Wrong visible serial build marker')
            expected_mount = 'FS mounted large IDE data disk' if profile=='large' else 'FS mounted IDE data disk'
            require(expected_mount in session.log.read_text(), 'Wrong mounted production profile')
            # Boot may add the ordinary examples. Wait for that genuine save.
            time.sleep(6); session.idle_saves('fresh production examples finish saving')
            record['lanes']['production-boot']='PASSED'
            session.launch('snap-probe.bex'); session.key('alt-left')
            session.observe_until(lambda s:s['phase']==0, 'unchanged peer is visible', keep='peer-ready')
            session.launch('pointer-window.bex'); session.key('alt-right'); session.move(1275,670)
            before = read_pair(session,font,'both-ready'); record['samples'].append(before)
            require(before['probe']['keys']==0, 'Peer input baseline is not fresh')
            session.launch('media player'); session.key('alt-ret')
            for _ in range(5): session.key('equal')
            time.sleep(6); session.idle_saves('arranged application session finishes saving before audio')
            session.launch('harbor-test.mp3')
            record['audio_started']=player_state(session,shell,'Playing','mp3-playing')
            session.key('ctrl-m'); session.move(1275,670)
            session.click(100,52); session.move(1275,670)
            trigger_deadline=time.monotonic()+90
            while time.monotonic()<trigger_deadline:
                session.key('t')
                state,_,_=session.observe()
                if state and state['phase']==1: break
                time.sleep(.15)
            else: raise AssertionError('Ordinary trigger was not admitted')
            session.observe_until(lambda s:s['phase']==1 and s['busy']>0,
                'ordinary write overlaps live IDE snapshot', seconds=90, keep='save-busy')
            for index in range(2):
                # Peer key and complete independent Pointer stroke are both
                # bounded by visible busy-phase samples of the same save.
                session.click(100,52); session.move(1275,670)
                prior = read_pair(session,font,f'peer-{index}-before')
                require(prior['probe']['phase']==1 and prior['probe']['busy']>0, 'Save ended before key sample')
                sent=time.monotonic(); session.key('a')
                after,_,wall=session.observe_until(lambda s:s['keys']>prior['probe']['keys'],
                    'peer PS/2 key visibly consumed', seconds=10, keep=f'peer-{index}-echo')
                require(after['phase']==1, 'Peer echo fell outside save overlap')
                record['input_to_visible_upper_bounds_ms'].append((wall-sent)*1000)
                session.click(750,52); session.move(1275,670)
                down=read_pair(session,font,f'pointer-{index}-before')
                require(down['probe']['phase']==1, 'Save ended before Pointer stroke')
                start=(20+index*30,60); end=(40+index*30,70)
                session.move(*point(down['pointer'],*start)); session.button(True)
                session.move(*point(down['pointer'],*end)); session.button(False); session.move(1275,670)
                up=read_pair(session,font,f'pointer-{index}-published')
                require(up['pointer']['done']==down['pointer']['done']+1 and
                    not up['pointer']['buttons'] and not up['pointer']['drag'], 'Pointer did not publish completed stroke')
                require(up['pointer']['sequence']>down['pointer']['sequence'], 'Pointer event stream did not advance')
                logical=reconstruct(session.observe()[1],native_viewport(tuple(up['pointer']['window'])),(160,100))
                require(tuple(logical[start[1],start[0]])==(42,167,200), 'Published original cyan ink is missing')
                require(up['probe']['phase']==1 and up['probe']['iterations']>down['probe']['iterations'] and
                        up['probe']['ticks']>down['probe']['ticks'], 'Peer did not progress throughout native publication during save')
                # A screenshot can contain a previously published peer frame.
                # Causally bracket the completed Pointer publication with a
                # NEW peer key, whose persisted guest tick must still be busy.
                session.click(100,52); session.move(1275,670)
                sent_after=time.monotonic(); session.key('a')
                echo,_,echo_wall=session.observe_until(lambda s:s['keys']>up['probe']['keys'],
                    'post-publication peer key visibly consumed', seconds=10, keep=f'pointer-{index}-post-key')
                require(echo['phase']==1 and echo['busy']>0, 'Pointer publication has no post-key save overlap')
                record['input_to_visible_upper_bounds_ms'].append((echo_wall-sent_after)*1000)
                post=read_pair(session,font,f'pointer-{index}-post-bracket')
                require(post['probe']['phase']==1 and post['probe']['busy']>0 and
                        post['probe']['keys']>up['probe']['keys'], 'Fresh post-publication bracket is missing')
                record['samples'] += [prior,down,up,post]; save_json(folder/'results.json',record)
            session.launch('media player')
            record['audio_after_overlap']=player_state(session,shell,'Playing','mp3-still-playing-after-overlap')
            session.key('ctrl-m'); session.move(1275,670)
            record['lanes']['peer-live-input']=record['lanes']['owned-pointer-draw-publication']='PASSED'
            session.observe_until(lambda s:s['phase']==3, 'unchanged peer report accepted',
                                  seconds=SAVE_TIMEOUT, keep='peer-report')
            session.idle_saves('report and snapshot durable serial completion')
            while time.monotonic()-record['audio_started']['wall']<AUDIO_SECONDS+4:
                session.keep_awake(); time.sleep(.1)
            session.launch('media player')
            # Existing player is restored maximized; no fullscreen toggle.
            record['audio_finished']=player_state(session,shell,'Finished','mp3-finished')
            session.normal_shutdown(); record['normal_shutdown']=True
        record['qemu_evidence']=copy_stopped_session(session,folder)
        serial=session.log.read_text(); record['serial_snapshot_jobs']=snapshot_jobs(serial)
        require('PANIC:' not in serial and 'result=error' not in serial and 'FS save failed' not in serial,
                'Production guest reported a failure')
        record['stopped_disk']=validate_saved(disk,original)
        require(record['stopped_disk']['report_words'][8]==4 and
                len(record['stopped_disk']['key_samples'])==4 and
                all(s['busy'] for s in record['stopped_disk']['key_samples']),
                'Four causal pre/post Pointer key brackets were not durably recorded during save')
        measured=[job for job in record['serial_snapshot_jobs'] if
                  job['begin_tick']>=record['stopped_disk']['report_words'][3]]
        require(measured and measured[0].get('result')=='durable', 'Measured snapshot did not finish durably')
        record['measured_snapshot']=measured[0]
        for sample in record['samples'][1:]:
            tick=sample['probe']['ticks']
            require(measured[0]['begin_tick']<=tick<=measured[0]['end_tick'], 'Visible concurrent sample outside measured snapshot')
        require(record['stopped_disk']['key_samples'] and all(
            measured[0]['begin_tick']<=s['tick']<=measured[0]['end_tick']
            for s in record['stopped_disk']['key_samples'] if s['busy']), 'Saved key sample outside measured snapshot')
        record['lanes']['save-overlap']=record['lanes']['stopped-disk']='PASSED'
        record['raw_audio_capture']=file_record(capture)
        record['audio']=verify_audio(capture,Path(manifest['media']['reference.s16']['path']))
        record['lanes']['mp3-complete-sb16']='PASSED'
        save_json(folder/'results.json',record)
        with Session(manifest['build_directory'],'gui-media-'+profile+'-reboot', image=boot, extra=(
            '-m',str(PROFILES[profile])+'M','-drive',f'file={disk},format=raw,index=0,if=ide,cache=writeback')) as reboot:
            reboot.boot(); reboot.launch('snap-probe.bex'); reboot.key('alt-ret')
            reboot.observe_until(lambda s:s['persisted']==MAGIC,'reboot reads report and trigger',keep='reboot-report')
            reboot.key('v')
            verified,_,_=reboot.observe_until(lambda s:s['phase']==5,'all original payload bytes verified by public SDK reads',
                                             seconds=180,keep='reboot-payloads')
            require(verified['verified']==len(manifest['profiles'][profile]['file_lengths']), 'Missing verified payloads')
            reboot.key('q'); reboot.idle_saves('reboot idle saves'); reboot.normal_shutdown()
        record['reboot_evidence']=copy_stopped_session(reboot,folder)
        record['reboot_stopped_disk']=validate_saved(disk,original)
        require(file_record(boot)['sha256']==manifest['build']['baseos.img']['sha256'], 'Exact production boot copy changed')
        verify_inputs(manifest)
        record['lanes']['reboot-full-payload']='PASSED'; record['passed']=True
    except BaseException as error:
        record['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
        if session and session.process.poll() is not None and 'qemu_evidence' not in record:
            record['qemu_evidence']=copy_stopped_session(session,folder)
        if reboot and reboot.process.poll() is not None and 'reboot_evidence' not in record:
            record['reboot_evidence']=copy_stopped_session(reboot,folder)
        raise
    finally:
        save_json(folder/'results.json',record)
    return record


def require_release_window(now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    # Reserve a conservative 90 minutes before this release's freeze. Normal
    # execution is estimated at 12–15 minutes, but failure bounds are longer.
    minute = now.hour*60+now.minute
    require(not (now.date() == datetime.date(2026,10,4) and 21*60+50 <= minute < 23*60+35),
            'Release reservation: admit the next guest run at or after 23:35 UTC')


def run(prepared,output,slot_grant):
    require(slot_grant=='GRANTED-BY-PARENT','Explicit exclusive QEMU slot grant is required')
    require_release_window()
    manifest=json.loads((prepared/'manifest.json').read_text()); verify_inputs(manifest)
    output.mkdir(parents=True,exist_ok=False)
    report=dict(status='RUNNING',prepared_manifest=file_record(prepared/'manifest.json'),
                profiles={profile:{'status':'NOT RUN','ram_mib':ram} for profile,ram in PROFILES.items()})
    save_json(output/'results.json',report)
    try:
        for profile in PROFILES:
            require_release_window()
            report['profiles'][profile]=run_profile(manifest,prepared,output/profile,profile)
            save_json(output/'results.json',report)
        report['status']='PASSED'
    except BaseException as error:
        report['status']='FAILED; EVIDENCE RETAINED';report['failure']=repr(error)
        for profile in PROFILES:
            partial=output/profile/'results.json'
            if partial.exists():report['profiles'][profile]=json.loads(partial.read_text())
        raise
    finally:save_json(output/'results.json',report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path)
    parser.add_argument('--build-log',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--prepared',type=Path)
    parser.add_argument('--exclusive-qemu-slot')
    args=parser.parse_args()
    if args.run:
        require(args.prepared is not None,'--prepared is required for guest execution')
        run(args.prepared.resolve(),args.output.resolve(),args.exclusive_qemu_slot)
    else:
        require(args.build is not None and args.build_log is not None,'Preparation requires --build and --build-log')
        prepare(args.build.resolve(),args.output.resolve(),args.build_log.resolve())
