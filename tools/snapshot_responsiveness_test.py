"""Production-QEMU main-loop snapshot test using only ordinary PS/2 and display.

Fresh default/large fixtures, an SDK-only native echo/counter, real desktop
rendering, normal serial diagnostics, every-source-sample SB16 comparison,
independently decoded durable volumes, and normal reboot full-payload reads.
No guest memory reads, debugger, test kernel, injected calls, or fault probes.
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
import traceback

import numpy as np
from PIL import Image
from build_app import build as build_app
from init_data import initialize
from large_audio_input_test import verify_complete_capture
from mp3_test import host_decode
from qemu_session import DesktopSession
from volume import data_layout, decode, encode_snapshot, load, resolve

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAGIC = 0x31505253
FIELDS = 'phase keys busy ticks iterations max_busy_gap persisted verified'.split()
COLORS = ((216, 58, 58), (242, 201, 76), (42, 167, 200), (168, 74, 192))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fnv(data):
    result = 2166136261
    for value in data:
        result = ((result ^ value) * 16777619) & 0xffffffff
    return result


def fixture(directory, profile, song, app, seed=None):
    layout = data_layout(profile)
    disk = directory / (profile + '.img')
    assert initialize(disk, profile=profile)
    lengths = [2 << 20, 2 << 20, 2 << 20, 1 << 20] if profile == 'default' else [16 << 20, 12 << 20]
    # Preserve the exact pre-reset workload for performance comparisons.
    payloads = [bytes(range(256)) * (length // 256) for length in lengths]
    manifest = [0x31464e4d, len(payloads)]
    for data in payloads:
        manifest += [len(data), fnv(data)]
    manifest += [0] * (16 - len(manifest))
    nodes = load(seed.read_bytes())[2] if seed else {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0),
        2: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0)}
    documents, programs = resolve(nodes, '/Documents'), resolve(nodes, '/Programs')
    entries = [(documents, 'trigger.bin', b'A'), (documents, 'probe.bin', b'P'),
               (documents, 'report.bin', bytes(1024)),
               (documents, 'manifest.bin', struct.pack('<16I', *manifest)),
               (programs, 'snap-probe.bex', app.read_bytes()),
               (documents, 'harbor-test.mp3', song.read_bytes())]
    entries += [(documents, f'payload{i}.bin', data) for i, data in enumerate(payloads)]
    for parent, name, content in entries:
        ident = next(i for i in range(layout.node_limit) if i not in nodes)
        nodes[ident] = dict(parent=parent, name=name, directory=0, app=0,
                            data=content, modified=123400 + ident)
    header, payload = encode_snapshot(nodes, layout, 1)
    raw = bytearray(disk.read_bytes()); start = layout.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    assert load(raw)[2] == nodes
    disk.write_bytes(raw)
    return disk, nodes, dict(payload_bytes=sum(lengths), initial_snapshot_bytes=len(payload),
                            file_lengths=lengths, manifest=manifest)


def snapshot_jobs(serial):
    jobs = []
    for kind, tick, generation, result in re.findall(
            r'FS snapshot (begin|end) tick=([0-9A-F]{8}) generation=([0-9A-F]{8})(?: result=(durable|error))?', serial):
        tick, generation = int(tick, 16), int(generation, 16)
        if kind == 'begin':
            jobs.append(dict(generation=generation, begin_tick=tick))
        else:
            candidates = [job for job in jobs if job['generation'] == generation and 'end_tick' not in job]
            assert len(candidates) == 1, ('Unpaired snapshot marker', kind, tick, generation)
            candidates[0].update(end_tick=tick, result=result,
                                 duration_seconds=((tick - candidates[0]['begin_tick']) & 0xffffffff) / 70)
    return jobs


def outstanding_snapshots(serial):
    return [job for job in snapshot_jobs(serial) if 'end_tick' not in job]


class Session(DesktopSession):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.events = []
        self.canvas = None
        self.frame_number = 0
        self.last_io = 0
        self.last_key = time.monotonic()

    def memory(self, *args, **kwargs):
        raise AssertionError('This verification forbids all guest-memory observation')

    def key(self, key, delay=.065):
        began = time.monotonic()
        self.last_key = began
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 25})
        self.events.append(dict(kind='key', key=key, wall=began))
        if delay:
            time.sleep(delay)
        return began

    def keep_awake(self):
        if time.monotonic() - self.last_key > 10:
            self.key('shift', delay=0)  # No app key/redraw; ordinary anti-idle input.

    def frame(self, keep=None):
        self.frame_number += 1
        target = self.directory / (keep or 'latest')
        path = target.with_suffix('.ppm')
        began = time.monotonic()
        self.command('screendump', {'filename': str(path), 'format': 'ppm'})
        dumped = time.monotonic()
        with Image.open(path) as image:
            pixels = np.array(image.convert('RGB'))
            if keep:
                image.save(target.with_suffix('.png'))
        if dumped - self.last_io >= 2:
            self.last_io = dumped
            stats = next(item['stats'] for item in self.command('query-blockstats')
                         if item.get('device') == 'ide0-hd0')
            self.events.append(dict(kind='blockstats', wall=dumped, **{
                key: stats[key] for key in ('rd_bytes', 'wr_bytes', 'rd_operations', 'wr_operations', 'flush_operations')}))
            (self.directory / 'blockstats-latest.json').write_text(json.dumps(self.events[-1]) + '\n')
        self.events.append(dict(kind='frame', number=self.frame_number, wall=began,
                                qmp_ms=(dumped - began) * 1000, kept=keep))
        return pixels, dumped

    def screenshot(self, name):
        self.frame(pathlib.Path(name).stem)
        return self.directory / name

    def observe(self, keep=None):
        pixels, dumped = self.frame(keep)
        if self.canvas is None:
            ys, xs = np.where(np.all(pixels == COLORS[0], axis=2))
            for y, x in zip(ys, xs):
                for scale in (1, 2, 3, 4):
                    if y + 99 * scale >= len(pixels) or x + 159 * scale >= pixels.shape[1]:
                        continue
                    if all(tuple(pixels[y + 2 * scale, x + (8 * i + 2) * scale]) == color
                           for i, color in enumerate(COLORS)):
                        if x and tuple(pixels[y, x - 1]) == COLORS[0]:
                            continue
                        if y and tuple(pixels[y - 1, x]) == COLORS[0]:
                            continue
                        self.canvas = (int(x), int(y), scale)
                        break
                if self.canvas:
                    break
        if self.canvas is None:
            return None, pixels, dumped
        x, y, scale = self.canvas
        values = []
        for row in range(8):
            value = 0
            for bit in range(32):
                color = tuple(pixels[y + (14 + row * 10) * scale, x + (bit * 5 + 2) * scale])
                if color == (255, 255, 255):
                    value |= 1 << bit
                elif color != (0, 0, 0):
                    raise AssertionError(('Canvas data was obscured or malformed', row, bit, color))
            values.append(value)
        return dict(zip(FIELDS, values)), pixels, dumped

    def wait_observe(self, predicate, message, seconds=90, keep=None, interval=.05):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.keep_awake()
            observed, pixels, wall = self.observe()
            if observed is not None and predicate(observed):
                if keep:
                    observed, pixels, wall = self.observe(keep)
                return observed, pixels, wall
            if self.process.poll() is not None or 'PANIC:' in self.log.read_text():
                raise AssertionError(message + '\n' + self.log.read_text())
            time.sleep(interval)
        raise AssertionError(message)

    def __exit__(self, kind, error, tb):
        if error is not None:
            try:
                self.screenshot('failure.png')
            except Exception:
                pass
        (self.directory / 'qmp-events.json').write_text(json.dumps(self.events, indent=2) + '\n')
        return super().__exit__(kind, error, tb)


def make_seed(build, directory):
    """Install authentic examples/preferences via an ordinary small-volume boot."""
    disk = directory / 'seed.img'
    assert initialize(disk)
    nodes = {i: dict(parent=parent, name=name, directory=1, app=0, data=b'', modified=0)
             for i, parent, name in ((0, -1, ''), (1, 0, 'Documents'), (2, 0, 'Programs'))}
    header, payload = encode_snapshot(nodes, data_layout(), 1)
    raw = bytearray(disk.read_bytes()); raw[512:1024] = header.ljust(512, b'\0')
    raw[1024:1024 + len(payload)] = payload; disk.write_bytes(raw)
    with Session(build, 'snapshot-seed', extra=[
            '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback']) as session:
        session.boot()
        started = time.monotonic()
        session.wait(lambda: time.monotonic() - started >= 10 and
                     'FS snapshot end' in session.log.read_text() and
                     not outstanding_snapshots(session.log.read_text()), 'small normal boot seed durable', 90)
        assert 'result=error' not in session.log.read_text()
        session.screenshot('seed.png')
    assert load(disk.read_bytes())[1] > 1
    return disk


def validate_saved(disk, original):
    raw = disk.read_bytes()
    slot, generation, nodes = load(raw)
    changing = {resolve(original, '/Documents/' + name) for name in ('trigger.bin', 'probe.bin', 'report.bin')}
    try:
        session_id = resolve(original, '/prefs/session')
    except ValueError:
        session_id = -1
    for ident, expected in original.items():
        actual = nodes[ident]
        if ident == session_id:
            continue  # The desktop legitimately persists its open windows.
        if ident in changing:
            fields = ('name', 'parent', 'directory', 'app')
            assert {k: actual[k] for k in fields} == {k: expected[k] for k in fields}
        else:
            assert actual == expected, ('Original payload/metadata changed', ident)
    assert nodes[resolve(nodes, '/Documents/trigger.bin')]['data'] == b'B'
    assert nodes[resolve(nodes, '/Documents/probe.bin')]['data'] == b'P'
    report = struct.unpack('<256I', nodes[resolve(nodes, '/Documents/report.bin')]['data'])
    assert report[:3] == (MAGIC, 1, 70)
    assert report[7] > 0 and report[8] > 0 and report[12] == 0 and report[13] == ord('B'), report[:16]
    assert report[3] <= report[4] <= report[5] <= report[6]
    key_samples = [dict(tick=report[16 + 2 * i], busy=bool(report[17 + 2 * i]))
                   for i in range(min(report[8], 64))]
    assert sum(sample['busy'] for sample in key_samples) >= 2, key_samples
    assert generation > 1
    valid = [dict(slot=i, generation=decoded[0]) for i in range(2)
             if (decoded := decode(raw, i)) is not None]
    return dict(slot=slot, generation=generation, valid_slots=valid, disk_sha256=sha(raw),
                report_words=list(report[:16]), key_samples=key_samples,
                lease_seconds=(report[6] - report[3]) / 70,
                observed_busy_seconds=(report[5] - report[4]) / 70,
                max_native_gap_ms=report[9] * 1000 / 70,
                max_native_gap_during_lease_ms=report[10] * 1000 / 70,
                original_payload_and_metadata_exact=True)


def durable_report(disk, serial):
    """A written CRC-valid header is not yet the final filesystem result."""
    completed = {job['generation'] for job in snapshot_jobs(serial) if job.get('result') == 'durable'}
    if not completed:
        return 0
    try:
        raw = disk.read_bytes()
        for slot in range(2):
            decoded = decode(raw, slot)
            if decoded is None or decoded[0] not in completed:
                continue
            generation, nodes = decoded
            data = nodes[resolve(nodes, '/Documents/report.bin')]['data']
            if data[:4] == struct.pack('<I', MAGIC):
                return generation
    except (ValueError, OSError):
        pass
    return 0


def run_profile(build, work, profile, song, reference, app, audio_seconds, seed=None, save_timeout=180):
    disk, original, details = fixture(work, profile, song, app, seed)
    capture = work / 'capture.wav'
    response_samples = []
    with Session(build, 'snapshot-' + profile, extra=[
            '-m', '128M', '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback',
            '-audiodev', f'wav,id=out,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=out']) as session:
        print(profile + ' QEMU evidence: ' + str(session.directory), flush=True)
        session.boot()
        assert not outstanding_snapshots(session.log.read_text()), 'Seed unexpectedly needed a boot save'
        session.launch('media player')
        for _ in range(5):
            session.key('equal')
        session.launch('harbor-test.mp3')
        audio_start = time.monotonic()
        session.launch('snap-probe.bex')
        initial, _, _ = session.wait_observe(lambda o: o['phase'] == 0, 'native app visible', keep='ready')
        for _ in range(16):
            session.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': -80}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': -80}}]})
            time.sleep(.01)
        time.sleep(.1)
        end = time.monotonic() + 90
        while time.monotonic() < end:
            session.key('t')
            observed, _, _ = session.observe()
            if observed and observed['phase'] == 1:
                break
            time.sleep(.15)
        else:
            raise AssertionError('Trigger never admitted')
        first, _, _ = session.wait_observe(lambda o: o['phase'] == 1 and o['busy'] > 0,
                                           'ordinary write observes live autosave lease', keep='busy')
        print(profile + ' live lease: ' + json.dumps(first), flush=True)
        for i in range(8):
            prior, _, _ = session.observe()
            if prior['phase'] != 1:
                break
            sent = session.key('a', delay=0)
            after, _, visible = session.wait_observe(lambda o: o['keys'] > prior['keys'], 'PS/2 echo visible',
                                                     seconds=10, keep='echo-' + str(i), interval=.005)
            response_samples.append(dict(index=i, before=prior, after=after,
                                         input_to_visible_ms=(visible - sent) * 1000))
            (work / 'input-progress.json').write_text(json.dumps(response_samples, indent=2) + '\n')
            time.sleep(.04)
            if i == 0:
                old, old_pixels, _ = session.observe('mouse-before')
                sent_mouse = time.monotonic()
                session.command('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': 80}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': 80}}]})
                moved_at = None
                for _ in range(100):
                    new, new_pixels, wall = session.observe()
                    changed = np.any(new_pixels != old_pixels, axis=2)
                    old_count = int(changed[0:24, 0:24].sum())
                    new_count = int(changed[75:112, 75:112].sum())
                    if old_count > 3 and new_count > 3:
                        moved_at = wall
                        break
                    time.sleep(.01)
                assert moved_at is not None, 'PS/2 cursor movement not visible in expected old/new regions'
                assert old['phase'] == new['phase'] == 1 and old['busy'] > 0
                mouse = dict(input_to_visible_ms=(moved_at - sent_mouse) * 1000,
                             changed_old_pixels=old_count, changed_new_pixels=new_count,
                             before=old, after=new)
                session.observe('mouse-after')
        assert len(response_samples) >= 2, response_samples
        released, _, _ = session.wait_observe(lambda o: o['phase'] in (2, 3), 'autosave lease released',
                                              seconds=save_timeout, keep='released')
        if released['phase'] == 2:
            session.key('r')
        session.wait_observe(lambda o: o['phase'] == 3, 'report RAM write accepted', seconds=save_timeout, keep='report')
        report_generation = 0
        checked_serial = ''
        def report_ready():
            nonlocal report_generation, checked_serial
            session.keep_awake()
            current_serial = session.log.read_text()
            if current_serial != checked_serial:
                checked_serial = current_serial
                report_generation = durable_report(disk, current_serial)
            return report_generation > 0
        session.wait(report_ready, 'report generation finishes complete verified durability', save_timeout)
        while time.monotonic() - audio_start < audio_seconds + 3:
            time.sleep(.2)
        session.screenshot('completed.png')
        serial = session.log.read_text()
        assert 'FS save failed' not in serial and 'PANIC:' not in serial
        assert ('FS mounted large IDE data disk' if profile == 'large' else 'FS mounted IDE data disk') in serial
        evidence = str(session.directory)
        captures = [event['qmp_ms'] for event in session.events if event['kind'] == 'frame']
    durable = validate_saved(disk, original)  # Definitive checks with QEMU stopped.
    jobs = snapshot_jobs(serial)
    starts = [job for job in jobs if job['begin_tick'] >= durable['report_words'][3]]
    assert starts and starts[0].get('result') == 'durable', jobs
    measured = starts[0]
    assert measured['begin_tick'] <= durable['report_words'][4] <= durable['report_words'][5] <= measured['end_tick']
    assert all(measured['begin_tick'] <= item['tick'] <= measured['end_tick']
               for item in durable['key_samples'] if item['busy'])
    pcm = verify_complete_capture(capture, reference)
    with Session(build, 'snapshot-' + profile + '-reboot', extra=[
            '-m', '128M', '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback']) as reboot:
        reboot.boot()
        reboot.launch('snap-probe.bex')
        reboot.wait_observe(lambda o: o['persisted'] == MAGIC, 'reboot reads saved report and trigger', keep='reboot-report')
        reboot.key('v')
        verified, _, _ = reboot.wait_observe(lambda o: o['phase'] == 5,
            'rebooted ordinary native reads hash every payload byte', seconds=180, keep='reboot-verified')
        assert verified['verified'] == len(details['file_lengths'])
        assert 'FS loaded from disk' in reboot.log.read_text() and 'PANIC:' not in reboot.log.read_text()
        reboot_evidence = str(reboot.directory)
    final = validate_saved(disk, original)
    return dict(profile=profile, ram_mib=128, **details, durable=durable, reboot_durable=final,
                qemu_evidence=evidence, reboot_evidence=reboot_evidence,
                serial_snapshot_jobs=jobs, measured_snapshot=measured, report_durable_generation=report_generation,
                key_response_upper_bounds_ms=[s['input_to_visible_ms'] for s in response_samples],
                key_response_samples=response_samples, mouse_response=mouse,
                initial_observation=initial, released_observation=released,
                max_screendump_qmp_ms=max(captures), audio=pcm,
                observation_scope='PS/2 input, ordinary SDK app output, framebuffer, serial; no guest memory reads')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--profile', choices=('default', 'large', 'both'), default='both')
    parser.add_argument('--work', type=pathlib.Path)
    parser.add_argument('--audio-seconds', type=int, default=45)
    parser.add_argument('--seed', type=pathlib.Path, help='Verified normal small-boot seed, otherwise create one')
    parser.add_argument('--save-timeout', type=int, default=180)
    args = parser.parse_args()
    work = args.work or pathlib.Path(tempfile.mkdtemp(prefix='baseos-snapshot-responsive-'))
    work.mkdir(exist_ok=True, parents=True)
    print('Evidence: ' + str(work), flush=True)
    (work / 'runner-at-launch.py').write_bytes(pathlib.Path(__file__).read_bytes())
    (work / 'app-at-launch.c').write_bytes((ROOT / 'tests/snapshot_responsive_app.c').read_bytes())
    result = dict(passed=False, source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  build=str(args.build.resolve()), results=[], save_timeout=args.save_timeout,
                  runner_sha256=sha((work / 'runner-at-launch.py').read_bytes()),
                  app_source_sha256=sha((work / 'app-at-launch.c').read_bytes()))
    try:
        app = work / 'snap-probe.bex'
        build_app(ROOT / 'tests/snapshot_responsive_app.c', app)
        song = work / 'harbor-test.mp3'
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-stream_loop', '-1',
                        '-i', str(ROOT / 'assets/examples/harbor.mp3'), '-t', str(args.audio_seconds),
                        '-codec:a', 'libmp3lame', '-b:a', '96k', '-write_xing', '0',
                        '-id3v2_version', '0', str(song)], check=True)
        reference = host_decode(song, work)
        result['audio_source'] = dict(original_sha256=sha((ROOT / 'assets/examples/harbor.mp3').read_bytes()),
                                    encoded_sha256=sha(song.read_bytes()), bytes=song.stat().st_size,
                                    reference_bytes=reference.stat().st_size)
        seed = args.seed or make_seed(args.build.resolve(), work)
        result['seed_sha256'] = sha(seed.read_bytes())
        profiles = ('default', 'large') if args.profile == 'both' else (args.profile,)
        for profile in profiles:
            directory = work / profile
            directory.mkdir()
            result['results'].append(run_profile(args.build.resolve(), directory, profile, song, reference,
                                                 app, args.audio_seconds, seed, args.save_timeout))
            (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        result['passed'] = True
    except BaseException as error:
        result['failure'] = dict(type=type(error).__name__, message=str(error), traceback=traceback.format_exc())
        raise
    finally:
        (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: ' + str(work / 'results.json'), flush=True)


if __name__ == '__main__':
    main()
