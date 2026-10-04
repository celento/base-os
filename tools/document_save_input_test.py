"""Production document-save acceptance through normal PS/2, display and serial.

Never observes guest memory or reads/writes an image while QEMU owns it.  Native
and exported bytes are independently decoded only after the guest stops.  A
bounded, read-only SDK client verifies all outputs and inert payloads on reboot.
Default/large require actual visible input while the matching save is pending;
the explicit floppy profile claims synchronous compatibility only.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import select
import shutil
import struct
import subprocess
import tempfile
import time
import traceback

from PIL import Image
import numpy as np
from build_app import build as build_app
from init_data import initialize
from layout import constants
from qemu_session import DesktopSession
from update_image import install_kernel
from volume import (FLOPPY_LAYOUT, data_layout, decode, encode_snapshot,
                    load, locked_image, resolve)

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SEED = pathlib.Path('/workspace/shared/snapshot-rep-default-attempt1/seed.img')
MANIFEST_MAGIC, MANIFEST_LIMIT, ENTRY_BYTES = 0x31434442, 4096, 136
OUTPUTS = ('/Documents/async.bwr', '/Documents/async.bsh', '/Documents/document.rtf',
           '/Documents/document.pdf', '/Documents/spreadsheet.csv', '/Documents/empty.csv')

EXPECTED_CODEC_SOURCE = r'''
#include <stdio.h>
#include <string.h>
#include "writer_codec.h"
#include "writer_pdf.h"
static WriterDoc doc;
static unsigned char output[16384];
void platform_poll(void) { }
static int save(const char *directory, const char *name, unsigned length) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s", directory, name);
    if (n < 0 || (unsigned)n >= sizeof path) return 1;
    FILE *file = fopen(path, "wb");
    if (!file) return 1;
    int bad = fwrite(output, 1, length, file) != length;
    return fclose(file) || bad;
}
int main(int argc, char **argv) {
    unsigned length = 0, pages = 0;
    if (argc != 2 || writer_plain_import(&doc, (const unsigned char *)"ALPHAZCDE", 9)) return 1;
    if (writer_rtf_export(&doc, output, sizeof output, &length) ||
        save(argv[1], "document.rtf", length)) return 2;
    if (writer_pdf_export(&doc, WRITER_PDF_LETTER, output, sizeof output, &length, &pages) ||
        pages != 1 || save(argv[1], "document.pdf", length)) return 3;
    return 0;
}
'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def snapshot_jobs(serial):
    jobs = []
    for line in serial.splitlines(keepends=True):
        if not line.endswith('\n') or not line.startswith('FS snapshot '):
            continue  # A concurrently written trailing record is not yet evidence.
        match = re.fullmatch(
            r'FS snapshot (begin|end) tick=([0-9A-F]{8}) generation=([0-9A-F]{8})'
            r'(?: result=(durable|error))?\r?\n', line)
        assert match is not None, ('Malformed complete snapshot record', line)
        kind, tick, generation, result = match.groups()
        tick, generation = int(tick, 16), int(generation, 16)
        if kind == 'begin':
            assert not result and not any(j['generation'] == generation for j in jobs), 'Repeated generation'
            jobs.append(dict(generation=generation, begin_tick=tick))
        else:
            matches = [j for j in jobs if j['generation'] == generation and 'end_tick' not in j]
            assert len(matches) == 1 and result, 'Unpaired or incomplete snapshot result'
            matches[0].update(end_tick=tick, result=result,
                              duration_seconds=((tick - matches[0]['begin_tick']) & 0xffffffff) / 70)
    return jobs


def pending_generations(serial):
    return [j['generation'] for j in snapshot_jobs(serial) if 'end_tick' not in j]


def normalized(text):
    return re.sub(r'\s+', '', text).upper()


class WhiteUiText:
    """Exact solid-white production UI glyph cores in screenshot pixels only.

    Proportional glyph advances and all 18 rows, including zero pixels, must
    match. This is reserved for the selected white-on-blue Cancel button that
    OCR commonly omits; it never reads the guest model or window state.
    """
    def __init__(self, font_path):
        raw = pathlib.Path(font_path).read_bytes()
        source = raw.decode('ascii')
        advances = re.search(r'ui_font_adv\[95\]\s*=\s*\{(.*?)\n\};', source, re.S)
        pixels = re.search(r'ui_font_px\[95\]\[144\]\s*=\s*\{(.*?)\n\};', source, re.S)
        assert advances and pixels, 'Production UI font layout changed'
        self.advances = [int(value) for value in re.findall(r'\d+', advances[1])]
        packed = np.array([int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', pixels[1])],
                          dtype=np.uint8)
        assert len(self.advances) == 95 and packed.size == 95 * 144
        packed = packed.reshape(95, 18, 8)
        self.glyphs = np.stack((packed >> 4, packed & 15), axis=-1).reshape(95, 18, 16) == 15
        self.font_sha256 = sha(raw)

    def template(self, text):
        if not text.strip() or any(not 32 <= ord(char) <= 126 for char in text):
            raise ValueError('Expected visible printable ASCII text')
        width = sum(self.advances[ord(char) - 32] for char in text) + 16
        template = np.zeros((18, width), dtype=bool)
        position = 0
        for char in text:
            index = ord(char) - 32
            template[:, position:position + 16] |= self.glyphs[index]
            position += self.advances[index]
        return template

    def find(self, pixels, text):
        mask = np.all(np.asarray(pixels)[:, :, :3] == (255, 255, 255), axis=2)
        template = self.template(text)
        height, width = mask.shape
        tw = template.shape[1]
        if height < 18 or width < tw:
            return []
        first = next(index for index, char in enumerate(text) if char != ' ')
        x0 = sum(self.advances[ord(char) - 32] for char in text[:first])
        rows = np.zeros((height, width - 15), dtype=np.uint16)
        expected = np.zeros(18, dtype=np.uint16)
        for bit in range(16):
            rows |= mask[:, bit:width - 15 + bit].astype(np.uint16) << (15 - bit)
            expected |= template[:, x0 + bit].astype(np.uint16) << (15 - bit)
        candidates = np.ones((height - 17, width - tw + 1), dtype=bool)
        for row in range(18):
            candidates &= rows[row:row + height - 17, x0:x0 + width - tw + 1] == expected[row]
        found = []
        for y, x in zip(*np.where(candidates)):
            if np.array_equal(mask[y:y + 18, x:x + tw], template):
                found.append([int(x), int(y)])
        return found


def native_writer(data):
    assert len(data) >= 18, 'Truncated BWR1'
    magic, version, flags, count, reserved = struct.unpack_from('<4sHHII', data)
    assert (magic, version, flags, reserved) == (b'BWR1', 1, 0, 0)
    assert count <= 32768 and len(data) == 18 + count * 3
    return dict(text=data[16:16 + count], style=data[16 + count:17 + count * 2],
                paragraph=data[17 + count * 2:])


def native_sheet(data):
    assert len(data) >= 16, 'Truncated BSH1'
    magic, version, flags, rows, cols, count = struct.unpack_from('<4sHHHHI', data)
    assert (magic, version, flags, rows, cols) == (b'BSH1', 1, 0, 128, 26)
    assert count <= rows * cols
    position, previous, records = 16, -1, {}
    for _ in range(count):
        assert position + 4 <= len(data)
        index, kind, length = struct.unpack_from('<HBB', data, position)
        position += 4
        assert previous < index < rows * cols and kind in (1, 2, 3) and length <= 95
        source = data[position:position + length]
        assert len(source) == length
        assert all(32 <= byte <= 126 or byte in (9, 10, 13) for byte in source)
        records[divmod(index, cols)] = (kind, source.decode('ascii'))
        position += length
        previous = index
    assert position == len(data)
    return records


def manifest_bytes(files):
    assert files and len(files) <= (MANIFEST_LIMIT - 16) // ENTRY_BYTES
    data = struct.pack('<4I', MANIFEST_MAGIC, 1, len(files), 0)
    for path, content in files:
        path = path.encode('ascii')
        assert path.startswith(b'/') and 1 < len(path) < 128 and b'\0' not in path
        assert all(32 <= byte <= 126 for byte in path) and len(content) <= 16 << 20
        data += struct.pack('<128sII', path, len(content), fnv(content))
    assert len(data) <= MANIFEST_LIMIT
    return data


def expected_outputs(work):
    """Predetermine every final byte BEFORE creating or booting the fixture.

    Production codecs supply deterministic RTF/PDF wire bytes, while independent
    host parsers/pdftotext still establish their semantics after the guest saves.
    """
    directory = work / 'expected-outputs'; directory.mkdir()
    source = directory / 'expected-codec.c'; source.write_text(EXPECTED_CODEC_SOURCE)
    executable = directory / 'expected-codec'
    compiler = shutil.which('clang') or shutil.which('cc')
    assert compiler, 'A host C compiler is required for predetermined export bytes'
    subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'src'), str(source), str(ROOT / 'src/writer_codec.c'),
                    str(ROOT / 'src/writer_pdf.c'), '-o', str(executable)], check=True, timeout=60)
    subprocess.run([str(executable), str(directory)], check=True, timeout=15)
    text = b'ALPHAZCD'
    native = struct.pack('<4sHHII', b'BWR1', 1, 0, len(text), 0) + text + bytes(2 * (len(text) + 1))
    sheet = struct.pack('<4sHHHHI', b'BSH1', 1, 0, 128, 26, 1) + struct.pack('<HBB', 0, 2, 6) + b'27.125'
    files = dict(zip(OUTPUTS, (native, sheet, (directory / 'document.rtf').read_bytes(),
                              (directory / 'document.pdf').read_bytes(), b'27.125\r\n', b'')))
    for path, data in files.items():
        (directory / pathlib.PurePosixPath(path).name).write_bytes(data)
    return files


class StoppedDisk:
    """A local invariant in addition to QEMU-compatible image locks."""
    def __init__(self, path):
        self.path, self.active = pathlib.Path(path), False

    def read(self):
        assert not self.active, 'Running-image reads are forbidden'
        with locked_image(self.path) as source:
            return source.read()

def fixture(work, profile, build, app, seed, expected):
    layout = FLOPPY_LAYOUT if profile == 'floppy' else data_layout(profile)
    disk = work / (profile + '.img')
    assert not disk.exists(), 'Use a fresh work directory; never overwrite an earlier fixture'
    with locked_image(seed) as source:
        seed_raw = source.read()
    nodes = copy.deepcopy(load(seed_raw)[2])
    dropped = []
    if profile == 'floppy':
        for ident in list(nodes):
            if len(nodes[ident]['data']) > layout.file_limit:
                dropped.append(nodes[ident]['name']); del nodes[ident]
        raw = bytearray(layout.sectors * 512)
        raw[:512] = (build / 'boot.bin').read_bytes()
        install_kernel(raw, (build / 'kernel.bin').read_bytes(), constants())
    else:
        assert initialize(disk, profile=profile)
        raw = bytearray(disk.read_bytes())
    lengths = {'default': [2 << 20, 2 << 20, 2 << 20, 1 << 20],
               'large': [16 << 20, 12 << 20], 'floppy': [4096]}[profile]
    documents, programs = resolve(nodes, '/Documents'), resolve(nodes, '/Programs')
    bulk = [(f'doc-payload{i}.bin', bytes(range(256)) * (length // 256))
            for i, length in enumerate(lengths)]
    manifest = manifest_bytes(list(expected.items()) + [('/Documents/' + name, data) for name, data in bulk])
    entries = [(documents, 'document-check.bin', manifest),
               (programs, 'document-check.bex', app.read_bytes())]
    entries += [(documents, name, data) for name, data in bulk]
    payloads = {}
    for parent, name, content in entries:
        assert not any(n['parent'] == parent and n['name'] == name for n in nodes.values())
        ident = next(i for i in range(layout.node_limit) if i not in nodes)
        nodes[ident] = dict(parent=parent, name=name, directory=0, app=0,
                            data=content, modified=123400 + ident)
        if name.startswith('doc-payload'):
            payloads[ident] = copy.deepcopy(nodes[ident])
    header, payload = encode_snapshot(nodes, layout, 1)
    start = layout.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    assert load(raw)[2] == nodes
    disk.write_bytes(raw)
    metadata = dict(payload_bytes=sum(lengths), payload_files=[
        dict(id=i, **{k: v for k, v in node.items() if k != 'data'},
             length=len(node['data']), sha256=sha(node['data']), fnv=fnv(node['data']))
        for i, node in payloads.items()], initial_snapshot_bytes=len(payload),
        seed_sha256=sha(seed_raw), dropped_oversize_seed_files=dropped,
        preseeded_manifest_sha256=sha(manifest), preseeded_manifest_bytes=len(manifest),
        disk_bytes=len(raw), guest_mib=128 if profile == 'large' else 64)
    return StoppedDisk(disk), payloads, metadata


class Session(DesktopSession):
    """Only ordinary QMP display/input; inherited memory routes cannot be used."""
    ALLOWED_COMMANDS = {'qmp_capabilities', 'send-key', 'screendump', 'query-blockstats'}

    def __init__(self, build, label, disk, profile, work, timeout=180):
        self.disk_guard, self.output = disk, work / label
        self.asynchronous = profile != 'floppy'
        self.events, self.frame_count, self.timeout = [], 0, timeout
        self.begin_observed, self.admissions = {}, {}
        self.last_key = time.monotonic()
        self.qmp_buffer, self.qmp_id = b'', 0
        assert not disk.active
        disk.active = True
        extra = ['-m', '128M' if profile == 'large' else '64M']
        if profile != 'floppy':
            extra += ['-drive', f'file={disk.path},format=raw,index=0,if=ide,cache=writeback']
        try:
            # Keep the shared harness unchanged, but bound this subclass's QMP
            # greeting, writes and complete-line reads (including partial JSON).
            self.build, self.layout = pathlib.Path(build).resolve(), constants()
            self.directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-' + label + '-'))
            self.log = self.directory / 'serial.log'
            image = disk.path if profile == 'floppy' else self.directory / 'boot.img'
            if profile != 'floppy':
                data = bytearray(self.layout['DISK_SECTORS'] * 512)
                data[:512] = (self.build / 'boot.bin').read_bytes()
                install_kernel(data, (self.build / 'kernel.bin').read_bytes(), self.layout)
                image.write_bytes(data)
            self.stderr = (self.directory / 'stderr.log').open('w')
            self.process = subprocess.Popen(['qemu-system-i386', '-vga', 'std', '-drive',
                f'file={image},format=raw,index=0,if=floppy', '-display', 'none', '-serial',
                f'file:{self.log}', '-qmp', 'stdio', '-no-reboot', *extra],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, bufsize=0)
            os.set_blocking(self.process.stdin.fileno(), False)
            os.set_blocking(self.process.stdout.fileno(), False)
            greeting = json.loads(self.qmp_line(time.monotonic() + 15))
            assert 'QMP' in greeting, greeting
            self.command('qmp_capabilities')
        except Exception:
            # An unsuccessful constructor may still have started QEMU.
            if hasattr(self, 'process'):
                super().close()
            disk.active = False
            raise

    def command(self, name, arguments=None):
        assert name in self.ALLOWED_COMMANDS, 'Only normal display/input QMP is permitted'
        deadline = time.monotonic() + 30
        self.qmp_id += 1
        message = json.dumps(dict(execute=name, arguments=arguments or {}, id=self.qmp_id)).encode() + b'\n'
        offset = 0
        while offset < len(message):
            remaining = deadline - time.monotonic()
            assert remaining > 0, 'QMP write timed out'
            _, writable, _ = select.select([], [self.process.stdin], [], remaining)
            assert writable, 'QMP write timed out'
            offset += os.write(self.process.stdin.fileno(), message[offset:])
        while True:
            response = json.loads(self.qmp_line(deadline))
            if 'event' in response:
                continue
            assert response.get('id') == self.qmp_id, ('Unexpected QMP response', response)
            if 'error' in response:
                raise RuntimeError(response)
            if 'return' in response:
                return response['return']

    def qmp_line(self, deadline):
        while b'\n' not in self.qmp_buffer:
            remaining = deadline - time.monotonic()
            assert remaining > 0, 'QMP complete-line read timed out'
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            assert readable, 'QMP complete-line read timed out'
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError('QEMU closed its QMP output')
            self.qmp_buffer += chunk
            assert len(self.qmp_buffer) < 1 << 20, 'Unbounded QMP record'
        line, self.qmp_buffer = self.qmp_buffer.split(b'\n', 1)
        return line

    def memory(self, *args, **kwargs):
        raise AssertionError('All guest-memory observation is forbidden')

    def serial(self):
        text = self.log.read_text() if self.log.exists() else ''
        observed = time.monotonic()
        assert 'PANIC:' not in text and 'result=error' not in text, text
        for job in snapshot_jobs(text):
            self.begin_observed.setdefault(job['generation'], observed)
        return text

    def key(self, key, delay=.08):
        began = time.monotonic()
        self.last_key = began
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': part}
                                          for part in key.split('-')], 'hold-time': 35})
        self.events.append(dict(kind='key', key=key, wall=began))
        if delay:
            time.sleep(delay)
        return began

    def keep_awake(self):
        if time.monotonic() - self.last_key > 10:
            self.key('shift', delay=.08)  # Wait beyond 35ms release before the next chord.

    def wait(self, predicate, message, seconds=30):
        def awake_predicate():
            self.keep_awake()
            return predicate()
        return super().wait(awake_predicate, message, seconds)

    def text(self, text):
        began = time.monotonic()
        super().text(text)
        return began

    def frame(self, name, ocr=True):
        self.frame_count += 1
        stem = f'{self.frame_count:03d}-{name}'
        ppm = self.directory / (stem + '.ppm')
        png = ppm.with_suffix('.png')
        began = time.monotonic()
        self.command('screendump', {'filename': str(ppm), 'format': 'ppm'})
        dumped = time.monotonic()  # OCR and image processing are outside this bound.
        serial = self.serial()
        with Image.open(ppm) as image:
            image.save(png)
            enlarged = self.directory / 'ocr-input.png'
            image.resize((image.width * 2, image.height * 2), Image.Resampling.NEAREST).save(enlarged)
        ppm.unlink()
        text = ''
        if ocr:
            text = subprocess.run(['tesseract', str(enlarged), 'stdout', '--psm', '11'],
                                  check=True, capture_output=True, text=True, timeout=25).stdout
            png.with_suffix('.txt').write_text(text)
        event = dict(kind='frame', name=name, screenshot=str(self.output / png.name),
                     started=began, dumped=dumped, qmp_ms=(dumped - began) * 1000,
                     pending_generations=pending_generations(serial), ocr=text)
        self.events.append(event)
        return event

    def crop_ocr(self, event, region):
        """Read additional regions of THESE pixels, never a later guest frame."""
        crops = event.setdefault('ocr_crops', [])
        if any(crop['region'] == region for crop in crops):
            return
        path = self.directory / pathlib.Path(event['screenshot']).name
        with Image.open(path) as image:
            width, height = image.size
            boxes = {
                'modal': (width // 4, height // 6, 3 * width // 4, 25 * height // 36),
                'footer': (0, 8 * height // 9, width, 169 * height // 180),
            }
            bounds = boxes[region]
            crop = image.crop(bounds)
            cropped_path = path.with_name(path.stem + '-ocr-' + region + '.png')
            crop.resize((crop.width * 2, crop.height * 2), Image.Resampling.NEAREST).save(cropped_path)
        text = subprocess.run(['tesseract', str(cropped_path), 'stdout', '--psm', '6'],
                              check=True, capture_output=True, text=True, timeout=25).stdout
        cropped_path.with_suffix('.txt').write_text(text)
        event.setdefault('ocr_full_frame', event['ocr'])
        crops.append(dict(region=region, bounds=list(bounds), scale=2, psm=6, ocr=text,
                          screenshot=str(pathlib.Path(event['screenshot']).parent / cropped_path.name)))
        event['ocr'] += '\n' + text
        path.with_suffix('.txt').write_text(event['ocr'])

    def complete_ocr(self, event, texts):
        missing = [text for text in texts if not self.has_text(event, text)]
        if not missing:
            return
        # Footer phrases are common omissions in sparse whole-screen OCR. All
        # other missing text gets the central dialog region first.
        footer_first = any(normalized(text).startswith(('UNSAVED', 'SAVEDTODISK', 'SAVEDNATIVE',
                                                        'CSVVALUESEXPORTED')) for text in missing)
        for region in (('footer', 'modal') if footer_first else ('modal', 'footer')):
            self.crop_ocr(event, region)
            if all(self.has_text(event, text) for text in texts):
                return

    def has_text(self, event, text):
        if normalized(text) in normalized(event['ocr']):
            return True
        if text != 'Cancel':
            return False
        matches = event.setdefault('glyph_matches', [])
        existing = next((item for item in matches if item['text'] == text), None)
        if existing is None:
            if not hasattr(self, 'white_ui_text'):
                self.white_ui_text = WhiteUiText(ROOT / 'src/font.h')
            path = self.directory / pathlib.Path(event['screenshot']).name
            with Image.open(path) as image:
                positions = self.white_ui_text.find(image.convert('RGB'), text)
            existing = dict(text=text, positions=positions,
                            font_sha256=self.white_ui_text.font_sha256,
                            method='exact UI alpha-15 cores against RGB(255,255,255), all zeros included',
                            template_size=[self.white_ui_text.template(text).shape[1], 18])
            matches.append(existing)
        return bool(existing['positions'])

    def visible(self, name, *texts, generation=None, since=None, absent=(), seconds=12):
        deadline = time.monotonic() + seconds
        while True:
            self.keep_awake()
            event = self.frame(name)
            self.complete_ocr(event, texts)
            content = normalized(event['ocr'])
            okay = all(self.has_text(event, text) for text in texts)
            okay = okay and all(normalized(t) not in content for t in absent)
            if generation is not None:
                assert generation in event['pending_generations'], (
                    'Required same-generation pending display was not observed', generation, event)
            if okay:
                if since is not None:
                    event['input_to_visible_upper_bound_ms'] = (event['dumped'] - since) * 1000
                return event
            if time.monotonic() >= deadline:
                raise AssertionError(('Visible text not recognized; screenshots retained', texts, absent, event))
            time.sleep(.08)

    def accepted_name_frame(self, *texts, generation=None, absent=()):
        """Reuse acceptance pixels so OCR does not consume another pending interval."""
        event = self.last_accepted_name_frame
        self.complete_ocr(event, texts)
        content = normalized(event['ocr'])
        assert all(self.has_text(event, text) for text in texts), ('Accepted frame text missing', texts, event)
        assert all(normalized(t) not in content for t in absent), ('Accepted dialog remained visible', event)
        if generation is not None:
            assert generation in event['pending_generations'], ('Accepted frame was not pending', generation, event)
        return event

    def idle(self):
        self.wait(lambda: not pending_generations(self.serial()), 'All serial boundaries durable', self.timeout)

    def new_job(self, prior):
        found = []
        def exists():
            nonlocal found
            found = [j for j in snapshot_jobs(self.serial()) if j['generation'] not in prior]
            return bool(found)
        self.wait(exists, 'A new save serial generation begins', self.timeout)
        assert len(found) == 1, ('Save could not be attributed uniquely', found)
        return found[0]['generation']

    def durable(self, generation):
        if not self.asynchronous:
            assert generation is None
            return dict(synchronous=True, serial_generation=None,
                        gate='Visible application success, stopped-disk bytes, and SDK reboot required')
        job = None
        def done():
            nonlocal job
            matches = [j for j in snapshot_jobs(self.serial()) if j['generation'] == generation]
            assert len(matches) == 1
            job = matches[0]
            return 'end_tick' in job
        self.wait(done, f'Exact generation {generation} becomes durable', self.timeout)
        assert job['result'] == 'durable'
        return dict(job, **self.admissions.get(generation, {}))

    def record_admission(self, generation, started):
        observed = self.begin_observed[generation]
        admission = dict(submit_key_wall=started, serial_begin_observed_wall=observed,
                         submit_to_serial_begin_upper_bound_ms=(observed - started) * 1000)
        assert observed >= started, 'A previous boundary cannot be attributed to this submission'
        self.admissions[generation] = admission
        self.events.append(dict(kind='operation', generation=generation, **admission))

    def submit(self, action):
        self.idle()
        prior = {j['generation'] for j in snapshot_jobs(self.serial())}
        started = action()
        if not self.asynchronous:
            return None
        generation = self.new_job(prior)
        self.record_admission(generation, started)
        return generation

    def bound_native_save(self, model, guarded=False, before=None):
        """Associate a native save only after its own visible pending outcome.

        A racing recovery snapshot may reject Ctrl+S. Its generation is retained
        as background evidence, never admitted as this application's save.
        """
        assert model in ('writer', 'sheet')
        assert not guarded or self.asynchronous
        pending = ('Saving changes', 'Cancel keeps') if guarded else (
            ('New edits stay private.',) if model == 'writer' else ('Saving sheet to disk',))
        busy = ('Disk saving', 'Retry Save shortly') if guarded else ('Disk is saving', 'retry shortly')
        success = 'Saved to disk.' if model == 'writer' else 'Saved native .bsh to disk.'
        assert before is not None and not all(self.has_text(before, text) for text in pending), (
            'Native admission needs a captured nonpending model baseline', before)
        deadline, attempt = time.monotonic() + self.timeout, 0
        while True:
            assert time.monotonic() < deadline, 'Native save remained busy'
            self.idle()
            prior = {job['generation'] for job in snapshot_jobs(self.serial())}
            attempt += 1
            if guarded:
                # Both the initial guard and a failed Save restore Cancel focus.
                # Tab chooses Save; blindly repeating Enter would cancel it.
                self.key('tab')
                started = self.key('ret')
            else:
                started = self.key('ctrl-s')
            self.serial()  # Observe a complete begin before image/OCR work.
            while True:
                assert time.monotonic() < deadline, 'Native save outcome was not observed'
                self.keep_awake()
                event = self.frame(f'{model}-native-submit-{attempt}')
                def has_all(texts):
                    return all(self.has_text(event, text) for text in texts)
                if not has_all(pending) and not has_all(busy) and not self.has_text(event, success):
                    self.crop_ocr(event, 'modal' if guarded else 'footer')
                accepted, rejected = has_all(pending), has_all(busy)
                assert not (accepted and rejected), ('Ambiguous native save outcome', event)
                if rejected:
                    self.events.append(dict(kind='native-save-rejected-busy', model=model, guarded=guarded,
                                            attempt=attempt, submit_key_wall=started,
                                            prior_generations=sorted(prior),
                                            background_generations=event['pending_generations'],
                                            before=before, frame=event))
                    before = event  # A rejection is an observed nonpending baseline.
                    self.idle()
                    if guarded:
                        before = self.visible('same-close-guard-before-retry', 'Save changes before closing', 'Cancel')
                    break  # Retry this specific rejected action, with fresh attribution.
                if accepted:
                    if not self.asynchronous:
                        continue  # A legacy save must finish visibly before proceeding.
                    fresh = set(event['pending_generations']) - prior
                    assert len(fresh) == 1, ('No unique fresh generation for model pending display', event, prior)
                    generation = fresh.pop()
                    self.record_admission(generation, started)
                    self.last_accepted_native_frame = event
                    self.events.append(dict(kind='native-save-accepted', model=model, guarded=guarded,
                                            attempt=attempt, generation=generation, before=before, frame=event))
                    return generation
                if self.has_text(event, success):
                    assert not self.asynchronous, ('Native save completed without a model-specific pending capture', event)
                    self.last_accepted_native_frame = event
                    return None
                time.sleep(.08)

    def name_dialog(self, shortcut, title, filename):
        self.idle()
        self.key(shortcut)
        self.visible('name-dialog', title)
        for _ in range(23):
            self.key('backspace')
        self.text(filename)
        self.visible('named-dialog', filename, title)
        deadline = time.monotonic() + self.timeout
        while True:
            self.idle()  # Opening a dialog may have raced a recovery boundary.
            prior = {j['generation'] for j in snapshot_jobs(self.serial())}
            started = self.key('ret')
            self.serial()  # Timestamp first observed begin before OCR processing.
            event = self.frame('name-submit-' + filename)
            content = normalized(event['ocr'])
            if normalized(title) not in content:
                # Sparse OCR can omit a complete dialog. Check its region from
                # the SAME capture before concluding that submission hid it.
                self.crop_ocr(event, 'modal')
                content = normalized(event['ocr'])
            if normalized(title) not in content:
                self.last_accepted_name_frame = event
                if not self.asynchronous:
                    return None
                generation = self.new_job(prior)
                self.record_admission(generation, started)
                return generation
            self.events.append(dict(kind='name-not-accepted', filename=filename, frame=event))
            assert 'DISKISSAVING' in content or 'RETRY' in content, (
                'Name dialog was not accepted; no blind Enter retry', event)
            assert time.monotonic() < deadline, 'Name submission remained busy'
            self.idle()
            # Never retry Enter after an accepted dialog has hidden itself.
            self.visible('same-dialog-before-retry', title, filename)

    def recovery_boundary(self, after_generation):
        # The desktop writes a later recovery snapshot once idle. No disk read
        # substitutes for its real serial completion while the guest is alive.
        if not self.asynchronous:
            # Legacy sync need not emit owned async markers. This waiting period
            # is explicitly NOT evidence of completion: stopped decoding and the
            # following ordinary reboot must prove the exact recovery bytes.
            time.sleep(15)
            self.frame('floppy-recovery-wait')
            return dict(wait_seconds=15, completion_not_inferred=True,
                        required_gate='Stopped image exact draft plus next reboot')
        def later():
            jobs = snapshot_jobs(self.serial())
            return any(j['generation'] != after_generation and
                       j['begin_tick'] >= next(x['begin_tick'] for x in jobs
                                              if x['generation'] == after_generation) and
                       j.get('result') == 'durable' for j in jobs) and not pending_generations(self.serial())
        self.wait(later, 'A later session recovery generation completes', self.timeout)
        return snapshot_jobs(self.serial())

    def __exit__(self, kind, error, tb):
        try:
            if error is not None:
                try:
                    self.frame('failure')
                except Exception as capture_error:
                    self.events.append(dict(kind='failure-capture-error', error=str(capture_error)))
            (self.directory / 'qmp-events.json').write_text(json.dumps(self.events, indent=2) + '\n')
        finally:
            try:
                super().__exit__(kind, error, tb)
            finally:
                self.disk_guard.active = False
                shutil.copytree(self.directory, self.output, dirs_exist_ok=True)


def verify_payloads(nodes, originals):
    for ident, original in originals.items():
        assert nodes.get(ident) == original, ('Immutable payload or metadata changed', ident)
    return [dict(path='/Documents/' + node['name'], bytes=len(node['data']), sha256=sha(node['data']))
            for node in originals.values()]


def stopped_snapshot(disk, originals, work, label):
    raw = disk.read()
    slot, generation, nodes = load(raw)
    evidence = dict(slot=slot, generation=generation, disk_sha256=sha(raw),
                    immutable_payloads=verify_payloads(nodes, originals), valid_slots=[
                        dict(slot=i, generation=result[0]) for i in range(2)
                        if (result := decode(raw, i)) is not None])
    # Preserve the actual image boundary before any later reboot or manifest edit.
    (work / (label + '.img')).write_bytes(raw)
    return nodes, evidence


def output_bytes(nodes, path):
    node = nodes[resolve(nodes, path)]
    assert not node['directory'] and not node['app'], path
    return node['data']


def verify_outputs(nodes, work):
    files = {path: output_bytes(nodes, path) for path in OUTPUTS}
    native = native_writer(files['/Documents/async.bwr'])
    assert native == dict(text=b'ALPHAZCD', style=bytes(9), paragraph=bytes(9)), native
    assert native_sheet(files['/Documents/async.bsh']) == {(0, 0): (2, '27.125')}
    spec = importlib.util.spec_from_file_location('independent_writer_codec', ROOT / 'tests/test_writer_codec.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    characters, insertion = module.rtf_semantics(files['/Documents/document.rtf'])
    assert characters == [(c, 0, 0, 24) for c in 'ALPHAZCDE'], characters
    assert insertion['style'] == insertion['alignment'] == 0 and insertion['size'] == 24
    pdf = work / 'document.pdf'; pdf.write_bytes(files['/Documents/document.pdf'])
    extracted = subprocess.run(['pdftotext', '-layout', str(pdf), '-'], check=True,
                               capture_output=True, text=True, timeout=30).stdout
    assert extracted.strip() == 'ALPHAZCDE', extracted
    assert files['/Documents/spreadsheet.csv'] == b'27.125\r\n'
    assert files['/Documents/empty.csv'] == b''
    output_directory = work / 'decoded-outputs'; output_directory.mkdir(exist_ok=True)
    for path, content in files.items():
        (output_directory / pathlib.PurePosixPath(path).name).write_bytes(content)
    return files, dict(native_writer='ALPHAZCD', exported_writer='ALPHAZCDE',
                       native_sheet={(str(k)): v for k, v in native_sheet(files['/Documents/async.bsh']).items()},
                       pdf_text=extracted, csv='27.125\r\n', empty_csv_bytes=0,
                       files={path: dict(bytes=len(content), sha256=sha(content), fnv=fnv(content))
                              for path, content in files.items()})


def source_evidence(build):
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()
    sources = sorted(path for root in ('src', 'sdk') for path in (ROOT / root).rglob('*') if path.is_file())
    hashes = {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in sources}
    build_info_path = build / 'build_info.h'
    build_info = build_info_path.read_text()
    revision = re.search(r'^#define BASEOS_BUILD_REVISION "([a-f0-9]+)"$', build_info, re.M)[1]
    dirty = int(re.search(r'^#define BASEOS_BUILD_DIRTY ([01])$', build_info, re.M)[1])
    assert not dirty, 'Guest acceptance requires a clean production build'
    # The collector itself may be new/uncommitted. Runtime code must match the
    # exact revision recorded by the frozen build, independently of that status.
    subprocess.run(['git', 'diff', '--exit-code', revision, '--', 'src', 'sdk'], cwd=ROOT,
                   check=True, capture_output=True)
    provenance_path = build.parent / 'HOST_RESULTS.json'
    provenance = json.loads(provenance_path.read_text()) if provenance_path.exists() else None
    build_hashes = {name: sha((build / name).read_bytes()) for name in ('boot.bin', 'kernel.bin', 'kernel.elf')
                    if (build / name).exists()}
    if provenance is not None:
        assert provenance['source_commit'] == revision
        assert provenance['production_build']['passed'] and not provenance['production_build']['build_dirty']
        assert all(provenance['hashes'][name] == value for name, value in build_hashes.items())
    return dict(revision=revision, collector_worktree_status=git('status', '--short'),
                build_info=build_info, build_info_sha256=sha(build_info.encode()),
                production_source_sha256=sha(json.dumps(hashes, sort_keys=True).encode()),
                production_files=hashes, build=str(build), build_hashes=build_hashes,
                frozen_build_provenance=None if provenance is None else dict(
                    source_commit=provenance['source_commit'], production_build=provenance['production_build'],
                    hashes=provenance['hashes'], evidence_path=str(provenance_path)),
                collector_sha256=sha(pathlib.Path(__file__).read_bytes()),
                verifier_source_sha256=sha((ROOT / 'tests/document_reboot_check.c').read_bytes()))


def writer_first(session, asynchronous):
    session.boot(); session.idle()
    # Prepare the other ordinary window before the measured save. Switching to
    # it and changing one character stays meaningful even on a fast IDE boundary.
    session.launch('editor'); session.key('alt-ret'); session.text('OTHER LIVE')
    session.visible('other-window-before-save', 'OTHER LIVE')
    session.launch('writer')
    session.key('alt-ret')  # Maximize this first-open window through normal input.
    session.text('ALPHA'); session.visible('writer-before-save', 'ALPHA')
    generation = session.name_dialog('ctrl-shift-s', 'Save document as', 'async.bwr')
    accepted = session.accepted_name_frame('ALPHA', 'Saving' if asynchronous else 'Saved',
                                           generation=generation if asynchronous else None,
                                           absent=('Save document as',))
    began = session.text('Z')
    private = session.visible('writer-private-edit', 'ALPHAZ',
                              generation=generation if asynchronous else None, since=began)
    session.key('ctrl-tab'); began = session.text('X')
    other = session.visible('other-window-live', 'OTHER LIVEX',
                            generation=generation if asynchronous else None, since=began)
    session.launch('writer'); session.visible('writer-returned', 'ALPHAZ')
    durable = session.durable(generation)
    session.visible('writer-submitted-baseline', 'ALPHAZ', 'Unsaved')
    recovery = session.recovery_boundary(generation)
    session.frame('writer-recovery-durable')
    return dict(save=durable, save_accepted=accepted, private_edit=private,
                other_window=other, recovery_jobs=recovery)


def writer_rest(session, asynchronous):
    session.boot(); session.idle(); session.launch('writer')
    session.visible('writer-recovered', 'ALPHAZ', 'Unsaved')
    session.text('C'); session.idle(); session.key('ctrl-w')
    guard_before = session.visible('close-guard-cancel-default', 'Save changes before closing', 'Cancel')
    if asynchronous:
        generation = session.bound_native_save('writer', guarded=True, before=guard_before)
        session.key('esc'); began = session.text('D')
        session.visible('close-cancel-private-edit', 'ALPHAZCD', generation=generation, since=began,
                        absent=('Saving changes...',))
        guarded = session.durable(generation)
    else:
        # The synchronous path has no observable pending cancel window. Cancel
        # the initial guard, edit, then exercise an ordinary durable Ctrl+S.
        session.key('esc'); session.text('D'); guarded = None
    native_before = session.visible('writer-before-latest-native', 'ALPHAZCD', 'Unsaved',
                                    absent=('New edits stay private.', 'Saving changes'))
    generation = session.bound_native_save('writer', before=native_before)
    native = session.durable(generation)
    session.visible('writer-latest-native-durable', 'ALPHAZCD', 'Saved to disk')
    session.text('E'); session.visible('writer-newer-export-source', 'ALPHAZCDE', 'Unsaved')
    exports = []
    for shortcut, title, name in (('ctrl-shift-e', 'Export rich text', 'document.rtf'),
                                 ('ctrl-shift-p', 'Export PDF pages', 'document.pdf')):
        generation = session.name_dialog(shortcut, title, name)
        if asynchronous:
            session.visible('export-accepted-' + name, 'ALPHAZCDE', 'Exporting',
                            generation=generation, absent=(title,))
        exports.append(dict(path=name, **session.durable(generation)))
        session.visible('export-keeps-native-dirty-' + name, 'ALPHAZCDE', 'Unsaved', absent=(title,))
    return dict(guard_save=guarded, latest_native=native, exports=exports)


def sheet_first(session, asynchronous):
    session.idle(); session.launch('spreadsheet'); session.key('alt-ret'); session.text('12.5')
    session.visible('sheet-uncommitted-before-save', '12.5')
    generation = session.name_dialog('ctrl-shift-s', 'Save spreadsheet as', 'async.bsh')
    if asynchronous:
        session.visible('sheet-save-accepted', '12.5', 'Saving sheet', generation=generation,
                        absent=('Save spreadsheet as',))
    began = session.text('27.125')
    edit = session.visible('sheet-new-uncommitted-edit', '27.125',
                           generation=generation if asynchronous else None, since=began)
    first = session.durable(generation)
    session.visible('sheet-pending-edit-survived-success', '27.125')
    recovery = session.recovery_boundary(generation)
    session.frame('sheet-private-recovery-durable')
    return dict(first_native=first, private_edit=edit, recovery_jobs=recovery)


def sheet_rest(session, asynchronous):
    session.boot(); session.idle(); session.launch('spreadsheet')
    native_before = session.visible('sheet-private-draft-recovered', '27.125', 'Recovered sheet draft',
                                    absent=('Saving sheet to disk',))
    generation = session.bound_native_save('sheet', before=native_before)
    second = session.durable(generation)
    session.visible('sheet-latest-native-durable', '27.125', 'Saved native .bsh to disk')
    generation = session.name_dialog('ctrl-shift-e', 'Export values', 'spreadsheet.csv')
    if asynchronous:
        session.visible('csv-accepted', 'Exporting CSV', generation=generation, absent=('Export values',))
    csv = session.durable(generation)
    session.visible('csv-durable-visible', 'CSV values exported')
    session.idle(); session.key('ctrl-n')
    session.visible('new-empty-sheet', 'Spreadsheet', absent=('Save changes before', '27.125'))
    generation = session.name_dialog('ctrl-shift-e', 'Export values', 'empty.csv')
    if asynchronous:
        session.visible('empty-csv-accepted', 'Exporting CSV', generation=generation, absent=('Export values',))
    empty = session.durable(generation)
    session.visible('empty-csv-durable-visible', 'CSV values exported')
    session.recovery_boundary(generation)
    session.frame('documents-final-durable')
    return dict(latest_native=second, csv=csv, empty_csv=empty)


def run(args):
    build, work, seed = args.build.resolve(), args.work.resolve(), args.seed.resolve()
    work.mkdir(parents=True, exist_ok=True)
    assert not (work / 'results.json').exists(), 'Use a fresh evidence directory'
    results = dict(passed=False, profile=args.profile, status='preparing',
                   responsiveness_required=args.profile != 'floppy', phases={}, source=source_evidence(build))
    def checkpoint():
        (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    checkpoint()
    try:
        app = work / 'document-check.bex'
        build_app(ROOT / 'tests/document_reboot_check.c', app)
        results['verifier_sha256'] = sha(app.read_bytes())
        expected = expected_outputs(work)
        results['predetermined_outputs'] = {path: dict(bytes=len(data), sha256=sha(data), fnv=fnv(data))
                                            for path, data in expected.items()}
        disk, payloads, metadata = fixture(work, args.profile, build, app, seed, expected)
        results['fixture'] = metadata
        results['status'] = 'prepared'; checkpoint()
        if args.prepare_only:
            return results
        asynchronous = args.profile != 'floppy'
        with Session(build, '01-writer-save', disk, args.profile, work, args.timeout) as session:
            results['phases']['writer_first'] = writer_first(session, asynchronous)
        nodes, first = stopped_snapshot(disk, payloads, work, 'first-stopped')
        assert native_writer(output_bytes(nodes, '/Documents/async.bwr'))['text'] == b'ALPHA'
        assert native_writer(output_bytes(nodes, '/prefs/writer-draft.bwr'))['text'] == b'ALPHAZ'
        first.update(native_text='ALPHA', recovered_draft='ALPHAZ')
        results['first_stopped'] = first; checkpoint()
        assert sha(disk.read()) == first['disk_sha256']
        with Session(build, '02-recovery-and-exports', disk, args.profile, work, args.timeout) as session:
            results['phases']['writer_rest'] = writer_rest(session, asynchronous)
            results['phases']['sheet_first'] = sheet_first(session, asynchronous)
        nodes, sheet_boundary = stopped_snapshot(disk, payloads, work, 'sheet-first-stopped')
        assert native_sheet(output_bytes(nodes, '/Documents/async.bsh')) == {(0, 0): (2, '12.5')}
        assert native_sheet(output_bytes(nodes, '/prefs/sheet-draft.bsh')) == {(0, 0): (2, '27.125')}
        sheet_boundary.update(native_cell='12.5', recovered_cell='27.125')
        results['sheet_first_stopped'] = sheet_boundary; checkpoint()
        assert sha(disk.read()) == sheet_boundary['disk_sha256']
        with Session(build, '03-sheet-recovery-and-exports', disk, args.profile, work, args.timeout) as session:
            results['phases']['sheet_rest'] = sheet_rest(session, asynchronous)
        nodes, final = stopped_snapshot(disk, payloads, work, 'final-stopped')
        files, outputs = verify_outputs(nodes, work)
        assert files == expected, 'Guest outputs differ from the pre-boot exact-byte oracle'
        results.update(final_stopped=final, output_checks=outputs); checkpoint()
        checked = list(files.items()) + [('/Documents/' + n['name'], n['data']) for n in payloads.values()]
        manifest = manifest_bytes(checked)
        assert output_bytes(nodes, '/Documents/document-check.bin') == manifest
        assert sha(manifest) == metadata['preseeded_manifest_sha256']
        results['reboot_manifest'] = dict(bytes=len(manifest), sha256=sha(manifest),
                                          count=len(checked), total_bytes=sum(len(data) for _, data in checked))
        results['reboot_start_disk_sha256'] = sha(disk.read())
        assert results['reboot_start_disk_sha256'] == final['disk_sha256'], (
            'Final reboot must use the untouched guest-produced stopped disk')
        with Session(build, '04-sdk-reboot-verification', disk, args.profile, work, args.timeout) as session:
            session.boot(); session.idle(); session.launch('writer')
            session.visible('writer-final-draft-recovered', 'ALPHAZCDE', 'Unsaved')
            session.launch('terminal'); session.key('alt-ret')
            session.text('start /Programs/document-check.bex'); session.key('ret')
            verified = session.visible('sdk-files-verified', 'DOCUMENT FILES VERIFIED',
                                       'Native task finished', seconds=args.timeout)
            results['sdk_reboot'] = verified
            session.idle()
        nodes, reboot = stopped_snapshot(disk, payloads, work, 'reboot-stopped')
        for path, data in checked:
            assert output_bytes(nodes, path) == data, ('Output changed after reboot', path)
        results['reboot_stopped'] = reboot
        assert source_evidence(build)['production_source_sha256'] == results['source']['production_source_sha256']
        assert source_evidence(build)['build_hashes'] == results['source']['build_hashes']
        results.update(passed=True, status='passed', claim='responsive IDE and reboot durability' if asynchronous
                       else 'synchronous floppy compatibility and reboot durability; no responsiveness claim')
    except Exception as error:
        results.update(status='failed', error=str(error), traceback=traceback.format_exc())
        raise
    finally:
        checkpoint()
        print(json.dumps({k: results[k] for k in ('passed', 'profile', 'status')}, indent=2), flush=True)
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--profile', choices=('default', 'large', 'floppy'), default='default')
    parser.add_argument('--work', type=pathlib.Path, required=True)
    parser.add_argument('--seed', type=pathlib.Path, default=DEFAULT_SEED)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--prepare-only', action='store_true', help='Build and validate fixture without launching QEMU')
    run(parser.parse_args())
