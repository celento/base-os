"""Bounded production-platform evidence helpers: PS/2, canvas and serial only."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

import numpy as np
from PIL import Image

from kernel_pack import unpack_kernel
from qemu_session import DesktopSession

ROOT = Path(__file__).resolve().parents[1]
MAGIC = 0x504c4154
COLORS = ((168, 74, 192), (216, 58, 58), (242, 201, 76), (42, 167, 200))
COMMON = 'magic process slot page keys loops ticks'.split()
NORMAL = 'file_result file_handle revision size read_hash sync_result operation pending foreign_result verified errors'.split()
CAPS = 'struct_size major minor features context user_bytes image_bytes stack_bytes chunk_bytes replace_bytes file_bytes'.split()
LIMITS = 'files_per_process files_total operations_per_process operations_total wait_ms hz processes path_bytes max_gap max_call errors'.split()
WAIT = 'wait_result wait_ticks operation sync_result wait_loops last_gap max_gap max_call wait_keys pending errors'.split()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def signed(value):
    return value - 0x100000000 if value & 0x80000000 else value


def semantic_payload(value):
    # These fields naturally advance between complete publications; ownership,
    # key acknowledgment, page identity and every actual result remain checked.
    changing={'loops','ticks','pending','max_gap','max_call','last_gap','wait_loops'}
    return {key:item for key,item in value.items() if key not in changing}


def provenance(build):
    """Record built source identity separately from the current harness checkout.

    A stale/mismatched raw and packed pair is a hard failure. No kernel rebuild
    is performed here: build ownership belongs to the integration coordinator.
    """
    build = Path(build).resolve()
    identity = json.loads((build / 'build_info.json').read_text())
    raw = (build / 'kernel.bin').read_bytes()
    packed = (build / 'kernel.packed').read_bytes()
    if unpack_kernel(packed) != raw:
        raise ValueError('kernel.bin and kernel.packed are from different builds')
    revision = identity['revision']
    if revision == 'unknown' or identity['dirty']:
        raise ValueError('Production validation requires an identified clean build')
    header = (build / 'build_info.h').read_text()
    if revision not in header or revision[:12].encode() not in raw:
        raise ValueError('Build identity header/raw kernel label mismatch')
    built_root = build.parent
    source_paths = subprocess.check_output([
        'git', '-C', str(built_root), 'ls-tree', '-r', '--name-only', revision, '--',
        'src', 'sdk', 'assets', 'third_party', 'examples', 'Makefile'], text=True).splitlines()
    runtime_sources = {}
    for name in source_paths:
        expected = subprocess.check_output(['git', '-C', str(built_root), 'show', f'{revision}:{name}'])
        if (built_root / name).read_bytes() != expected:
            raise ValueError('Runtime source differs from built revision: ' + name)
        runtime_sources[name] = sha(expected)
    return dict(built_source=identity, build_directory=str(build), runtime_source_sha256=runtime_sources,
                harness_revision=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                harness_status=subprocess.check_output(['git', '-C', str(ROOT), 'status', '--short'], text=True),
                artifacts={name: dict(bytes=(build / name).stat().st_size, sha256=sha((build / name).read_bytes()))
                           for name in ('boot.bin', 'kernel.bin', 'kernel.packed', 'kernel.elf', 'build_info.h', 'build_info.json')},
                raw_packed_roundtrip=True)


def decode_canvas(pixels):
    """Find one fully published 320x200 native canvas at either normal scale."""
    ys, xs = np.where(np.all(pixels == COLORS[0], axis=2))
    for y, x in zip(ys, xs):
        if x and tuple(pixels[y, x - 1]) == COLORS[0]:
            continue
        if y and tuple(pixels[y - 1, x]) == COLORS[0]:
            continue
        for scale in (1, 2):
            if y + 200 * scale > pixels.shape[0] or x + 320 * scale > pixels.shape[1]:
                continue
            if not all(tuple(pixels[y + 2 * scale, x + (8 * i + 2) * scale]) == color
                       for i, color in enumerate(COLORS)):
                continue
            values = []
            for row in range(18):
                value = 0
                for bit in range(32):
                    color = tuple(pixels[y + (14 + row * 10) * scale, x + (bit * 5 + 2) * scale])
                    if color == (255, 255, 255):
                        value |= 1 << bit
                    elif color != (0, 0, 0):
                        break
                else:
                    values.append(value)
                    continue
                break
            if len(values) != 18 or values[0] != MAGIC or values[3] not in (0, 1, 2, 3):
                continue
            fields = COMMON + (NORMAL if values[3] == 0 else CAPS if values[3] == 1 else LIMITS if values[3] == 2 else WAIT)
            result = dict(zip(fields, values))
            for field in ('file_result', 'sync_result', 'foreign_result', 'wait_result'):
                if field in result:
                    result[field] = signed(result[field])
            return result, (int(x), int(y), scale)
    return None, None


def snapshot_jobs(serial):
    jobs = []
    for kind, tick, generation, result in re.findall(
            r'FS snapshot (begin|end) tick=([0-9A-F]{8}) generation=([0-9A-F]{8})(?: result=(durable|error))?', serial):
        tick, generation = int(tick, 16), int(generation, 16)
        if kind == 'begin':
            jobs.append(dict(begin_tick=tick, generation=generation))
        else:
            matches = [job for job in jobs if job['generation'] == generation and 'end_tick' not in job]
            if len(matches) != 1:
                raise ValueError('Unpaired serial snapshot completion')
            matches[0].update(end_tick=tick, result=result, seconds=((tick - matches[0]['begin_tick']) & 0xffffffff) / 70)
    return jobs


class PlatformSession(DesktopSession):
    ALLOWED = {'qmp_capabilities', 'send-key', 'screendump', 'input-send-event',
               'query-blockstats', 'query-status'}

    def __init__(self, *args, **kwargs):
        self.events = []
        self.last_key = time.monotonic()
        self.last_io = 0
        self.transition_count = 0
        super().__init__(*args, **kwargs)

    def command(self, name, arguments=None):
        if name not in self.ALLOWED:
            raise AssertionError('Only PS/2 input and bounded read-only evidence are permitted: ' + name)
        return super().command(name, arguments)

    def memory(self, *_args, **_kwargs):
        raise AssertionError('Guest memory is not observed by this platform runner')

    def key(self, key, delay=.065):
        began = time.monotonic()
        self.last_key = began
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part} for part in key.split('-')], 'hold-time': 25})
        self.events.append(dict(kind='key', key=key, wall=began))
        if delay:
            time.sleep(delay)
        return began

    def frame(self, keep=None):
        path = self.directory / 'latest.ppm'
        began = time.monotonic()
        self.command('screendump', {'filename': str(path), 'format': 'ppm'})
        wall = time.monotonic()
        with Image.open(path) as image:
            pixels = np.array(image.convert('RGB'))
            if keep:
                image.save(self.directory / (keep + '.png'))
        self.events.append(dict(kind='frame', wall=wall, qmp_ms=(wall - began) * 1000, keep=keep))
        if wall - self.last_io >= 2:
            self.last_io = wall
            for entry in self.command('query-blockstats'):
                if entry.get('device') == 'ide0-hd0':
                    stats = entry['stats']
                    self.events.append(dict(kind='blockstats', wall=wall, **{
                        key: stats[key] for key in ('rd_bytes', 'wr_bytes', 'rd_operations',
                                                   'wr_operations', 'flush_operations')}))
        return pixels, wall

    def observe(self, keep=None):
        pixels, wall = self.frame(keep)
        observed, canvas = decode_canvas(pixels)
        if observed:
            self.events.append(dict(kind='canvas', wall=wall, canvas=canvas, **observed))
        return observed, pixels, wall

    def until(self, predicate, message, seconds=120, keep=None):
        end = time.monotonic() + seconds
        candidate = None
        while time.monotonic() < end:
            if time.monotonic() - self.last_key > 10:
                self.key('shift', 0)
            value, pixels, wall = self.observe()
            if value and predicate(value):
                if candidate and semantic_payload(candidate[0]) == semantic_payload(value):
                    if keep:
                        # Save the exact accepted observation, not a new possibly
                        # transitional screendump requested after acceptance.
                        Image.fromarray(pixels).save(self.directory / (keep + '.png'))
                    return value, pixels, wall
                if candidate:
                    self.transition_count += 1
                    prefix = f'transition-{self.transition_count:03d}'
                    Image.fromarray(candidate[1]).save(self.directory / (prefix + '-before.png'))
                    Image.fromarray(pixels).save(self.directory / (prefix + '-after.png'))
                    self.events.append(dict(kind='semantic-transition', wall=wall, prefix=prefix,
                                            before=candidate[0], after=value))
                candidate = value, pixels, wall
            else:
                candidate = None
            if self.process.poll() is not None or 'PANIC:' in self.log.read_text():
                raise AssertionError(message + '\n' + self.log.read_text())
            time.sleep(.04)
        raise AssertionError(message)

    def page(self, page):
        before = self.until(lambda o: True, 'native client before page selection')[0]
        self.key(str(page))
        return self.until(lambda o: o['process'] == before['process'] and
                          o['keys'] > before['keys'] and o['page'] == page,
                          'visible page ' + str(page))[0]

    def focus(self, process):
        for _ in range(9):
            observed, _, _ = self.observe()
            if observed and observed['process'] == process:
                # Published pixels can lag queued desktop shortcuts. A harmless
                # echo consumed by this exact owner proves actual input focus;
                # never queue another Ctrl+Tab based on a stale framebuffer.
                before = observed['keys']
                self.key('a', delay=.15)
                end = time.monotonic() + 2
                while time.monotonic() < end:
                    after, _, _ = self.observe()
                    if after and after['process'] == process and after['keys'] > before:
                        return after
                    time.sleep(.05)
            self.key('ctrl-tab', delay=.5)
        raise AssertionError('Could not focus native owner ' + str(process))

    def start_client(self):
        # Ctrl+N opens a new Terminal when one is focused. Launcher alone reuses
        # a finished Terminal, so use the ordinary Terminal command explicitly.
        self.key('ctrl-n')
        self.text('start /Programs/platform.bex')
        self.key('ret')
        self.key('alt-ret')
        return self.until(lambda o: o['page'] == 0, 'new platform client visible')[0]

    def __exit__(self, kind, error, tb):
        if error:
            try:
                self.frame('failure')
            except Exception:
                pass
        (self.directory / 'events.json').write_text(json.dumps(self.events, indent=2) + '\n')
        return super().__exit__(kind, error, tb)
