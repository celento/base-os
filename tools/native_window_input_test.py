"""Prepared production owned-window collector; guest execution is explicit.

Default mode ONLY builds ordinary app/data fixtures and records immutable inputs.
After an explicit QEMU grant: --run --output PREPARED_DIRECTORY. No kernel source
is patched or rebuilt. Live observations are PS/2, screenshots and serial only;
volumes are decoded only after QEMU exits. Failures and unexecuted lanes survive.
The runner has not been guest-qualified merely because preparation/tests pass.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

import numpy as np
from PIL import Image
from build_app import build as build_app, _bex2_header
from init_data import initialize
from kernel_pack import unpack_kernel
from layout import constants
from native_pointer_input_test import Font, Session, COLORS, FROZEN, require, sha256
from volume import data_layout, encode_snapshot, load, resolve

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = '/Documents/window-save.txt'
DOCUMENT_BYTES = 16384
HOSTED_SHA256 = 'ef7bd7ab6a39e197808cac22e68951044d69527e96f76b5346cc4c7205b65248'
PROFILES = {'default': 64, 'large': 128}
CORE_ARTIFACTS = ('baseos.img', 'kernel.bin', 'kernel.packed', 'kernel.elf', 'boot.bin')
LANES = ('pointer-model', 'launch-dispatch', 'output-log', 'owned-lifetime-pages',
         'durable-document', 'cold-reboot', 'old-kernel-refusal')
# These require separate UI/capacity collectors and must never be implied by a
# completed subset. Existing normal pointer/publication/save suites still apply.
SEPARATE_GATES = ['finite process/window/page admission matrix', 'Monitor Show/Stop and stale row actions',
                  'small-window fractional-downscale full-frame comparison',
                  'built-in Writer/Sheet/Todo/Calendar/preferences regression lanes']


def document_bytes():
    return bytes(10 if i % 80 == 79 else 32 + (i * 17 + 31) % 95 for i in range(DOCUMENT_BYTES))


def file_record(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=sha256(data))


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def native_viewport(window, logical=(160, 100)):
    """Exact published native-client aspect fit, from ordinary WM dimensions."""
    x, y, w, h = window
    sw, sh = logical
    cw, ch = w - 18, h - 80
    require(cw > 0 and ch > 0 and sw > 0 and sh > 0, 'Empty native client')
    vw, vh = cw, sh * cw // sw
    if vh > ch:
        vh, vw = ch, sw * ch // sh
    return x + 9 + (cw - vw)//2, y + 71 + (ch - vh)//2, vw, vh


def arranged_window(image, side=None):
    h, w = image.shape[:2]
    return (w//2 if side == 'right' else 2, 38, w//2 - 4 if side else w - 4, h - 84)


def resample(logical, width, height):
    h, w = logical.shape[:2]
    return logical[(np.arange(height)*h//height)[:, None], (np.arange(width)*w//width)[None, :]]


def reconstruct(image, viewport, logical):
    """Recover only a fully visible upscale, checking every replicated pixel.

    Logical cells can have unequal sizes. No OCR, interpolation or guessing is
    used. Small downscales are intentionally excluded from status decoding.
    """
    x, y, w, h = map(int, viewport)
    sw, sh = logical
    require(w >= sw and h >= sh, 'Status decoding needs at least one pixel per logical cell')
    block = image[y:y+h, x:x+w]
    require(block.shape == (h, w, 3), 'Native viewport is clipped')
    xs = (np.arange(sw)*w + sw-1)//sw
    ys = (np.arange(sh)*h + sh-1)//sh
    result = block[ys[:, None], xs[None, :]]
    # Pointer cursors may cover canvas pixels. Do not silently accept them.
    require(np.array_equal(block, resample(result, w, h)), 'Canvas is occluded or not exact nearest-neighbor output')
    return result


class NativeFont(Font):
    def pointer(self, image, window, logical=(160, 100)):
        viewport = native_viewport(window, logical)
        pixels = reconstruct(image, viewport, logical)
        state = self.decode(pixels, (0, 0), 1)
        require(state['logical'] == list(logical), 'Unexpected published logical size')
        state.update(origin=list(viewport[:2]), viewport=list(viewport[2:]), window=list(window))
        state.pop('scale')
        return state

    def document(self, image, window):
        viewport = native_viewport(window)
        pixels = reconstruct(image, viewport, (160, 100))
        rows = [self.line(pixels, (0, 0), 1, y, color, 38)
                for y, color in ((2, 8), (9, 7), (16, 7), (23, 7), (30, 6), (37, 8))]
        patterns = (r'WINDOW DOC V1', r'STATE=(\d+)', r'SAVE=(\d+) VERIFY=(\d+)',
                    r'OWN=(\d+) FREE=(\d+)', r'SLOT=(\d+) PHASE=(\d+)', r'SLEEP=(\d+) RESULT=(-?\d+)')
        matches = [re.fullmatch(p, row) for p, row in zip(patterns, rows)]
        require(all(matches), 'Unexpected document canvas rows: ' + repr(rows))
        nums = [int(x) for match in matches for x in match.groups()]
        return dict(zip(('state', 'saved', 'verified', 'owned', 'free', 'slot', 'phase', 'sleep', 'result'), nums),
                    rows=rows, origin=list(viewport[:2]), viewport=list(viewport[2:]), window=list(window))


def point(state, x, y):
    sw, sh = state.get('logical', [160, 100])
    w, h = state['viewport']
    # First screen pixel whose authoritative floor transform maps to this cell.
    return (state['origin'][0] + (x*w + sw-1)//sw,
            state['origin'][1] + (y*h + sh-1)//sh)


class ShellFont:
    """Source bitmap matcher for known shell text at an observed UI origin."""
    def __init__(self):
        source = (ROOT / 'src/font.h').read_text()
        self.glyphs = {}; self.levels = {}
        for kind, width in (('edit', 8), ('ui', 16)):
            match = re.search(r'\b' + kind + r'_font_px\[95\]\[\d+\]\s*=\s*(\{.*?\});', source, re.S)
            require(match is not None, 'Missing shell font table')
            data = np.array(ast.literal_eval(match[1].replace('{', '[').replace('}', ']')), dtype=np.uint8)
            unpacked = np.empty((95, 18, width), dtype=np.uint8)
            unpacked[:, :, ::2] = (data.reshape(95, 18, width//2) >> 4)
            unpacked[:, :, 1::2] = data.reshape(95, 18, width//2) & 15
            self.levels[kind] = unpacked
            self.glyphs[kind] = unpacked > 0
        match = re.search(r'ui_font_adv\[95\]\s*=\s*(\{.*?\});', source, re.S)
        self.advance = ast.literal_eval(match[1].replace('{', '[').replace('}', ']'))

    def bitmap(self, text, kind='edit', levels=False):
        widths = [8 if kind == 'edit' else self.advance[ord(c)-32] for c in text]
        canvas = np.zeros((18, sum(widths) + (8 if kind == 'ui' else 0)), dtype=np.uint8 if levels else bool)
        x = 0
        for c, advance in zip(text, widths):
            glyph = (self.levels if levels else self.glyphs)[kind][ord(c)-32]
            stop = min(canvas.shape[1], x+glyph.shape[1])
            canvas[:, x:stop] = np.maximum(canvas[:, x:stop], glyph[:, :stop-x])
            x += advance
        return canvas[:, :sum(widths)]

    def matches(self, image, x, y, text, kind='edit'):
        mask = self.bitmap(text, kind)
        patch = image[y:y+18, x:x+mask.shape[1]]
        if patch.shape != (18, mask.shape[1], 3):
            return False
        observed = np.any(patch != patch[0, 0], axis=2)
        if kind == 'edit':
            return bool(np.array_equal(observed, mask))
        # Low-alpha UI text can quantize to its background in the indexed
        # palette. All substantial ink and all transparent pixels are exact.
        levels = self.bitmap(text, kind, levels=True)
        return bool(np.all(~observed[levels == 0]) and np.all(observed[levels >= 3]))

    def contains(self, image, region, text, kind='edit'):
        x, y, w, h = region
        mask = self.bitmap(text, kind)
        # Bounded exact search, used only for visible diagnostic text. Glyph
        # positions are not guessed from OCR or private window memory.
        if not np.any(mask):
            return False
        iy, ix = np.argwhere(mask)[0]
        background = np.array([32, 32, 32], dtype=np.uint8)
        binary = np.any(image != background, axis=2)
        # Match one whole informative row first; this avoids calling the full
        # 18-row comparison for every foreground pixel in a desktop screenshot.
        row_mask = mask[iy]
        for top in range(y, y+h-17):
            row = binary[top+iy, x:x+w]
            if len(row) < len(row_mask):
                continue
            windows = np.lib.stride_tricks.sliding_window_view(row, len(row_mask))
            candidates = np.flatnonzero(np.all(windows == row_mask, axis=1))
            for left in candidates:
                if self.matches(image, x+int(left), top, text, kind):
                    return True
        return False


def declared_pages(data):
    h = _bex2_header(data)
    return (h[6]+4095)//4096 + (h[9]+4095)//4096 + h[10]//4096 + h[11]//4096 + 2


def verify_build_directory(build):
    """Prove the exact boot image contains the recorded packed/raw kernel."""
    build = Path(build); layout = constants()
    image = (build/'baseos.img').read_bytes(); boot = (build/'boot.bin').read_bytes()
    packed = (build/'kernel.packed').read_bytes(); raw = (build/'kernel.bin').read_bytes()
    sector = layout['SECTOR_SIZE']; first = layout['KERNEL_PRIMARY_SECTORS']*sector
    tail = layout['KERNEL_EXT_LBA']*sector
    require(len(image) == layout['DISK_SECTORS']*sector and len(boot) == sector,
            'Production boot artifacts have inconsistent extents')
    require(image[:sector] == boot, 'Held boot image differs from recorded boot.bin')
    installed = image[sector:sector+min(first, len(packed))]
    if len(packed) > first:
        installed += image[tail:tail+len(packed)-first]
    require(installed == packed, 'Held boot image differs from recorded kernel.packed')
    require(unpack_kernel(packed, layout) == raw, 'Packed kernel differs from recorded kernel.bin')
    return dict(boot_matches=True, packed_matches=True, raw_matches=True)


def make_volume(path, profile, apps):
    require(initialize(path, profile=profile), 'Refusing to overwrite fixture volume')
    nodes = {0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
             1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0),
             2: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0)}
    for name, data in apps.items():
        nodes[len(nodes)] = dict(parent=1, name=name, directory=0, app=0, data=data, modified=1)
    for slot in range(1, 9):
        nodes[len(nodes)] = dict(parent=2, name=f'counter-{slot}.txt', directory=0, app=0,
                                data=f'{100+slot}\n'.encode(), modified=1)
    layout = data_layout(profile)
    header, payload = encode_snapshot(nodes, layout, 1)
    with path.open('r+b') as output:
        output.seek(layout.lbas[0]*512); output.write(header.ljust(512, b'\0')); output.write(payload)
    return file_record(path)


def prepare(build, output, hosted_pointer, profiles=('default', 'large'), old_kernel=None):
    output.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema=1, status='PREPARING; NO GUEST RUN', profiles={}, apps={}, source={},
                    lanes={lane: 'NOT RUN' for lane in LANES}, separate_gates=SEPARATE_GATES,
                    observation_policy='QMP PS/2, screenshots, serial; stopped-volume reads only',
                    prohibited=['guest-memory access', 'debugger', 'fault/fuzz probes', 'live-volume reads'],
                    preparation_only=True, runner_guest_qualified=False)
    save_json(output/'manifest.json', manifest)
    try:
        manifest['build'] = {name: file_record(build/name) for name in CORE_ARTIFACTS}
        if (build/'build_info.json').exists():
            manifest['build']['build_info.json'] = file_record(build/'build_info.json')
            manifest['build_info'] = json.loads((build/'build_info.json').read_text())
        manifest['build_directory'] = str(build.resolve())
        manifest['revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        manifest['source_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT))
        source_paths = subprocess.check_output(['git', 'ls-files', 'src', 'sdk', 'assets', 'examples', 'Makefile', 'tools'],
                                               cwd=ROOT, text=True).splitlines()
        source_paths += ['tools/native_window_input_test.py', 'tests/native_window_document_app.c']
        for name in sorted(set(source_paths)):
            if (ROOT/name).is_file():
                manifest['source'][name] = file_record(ROOT/name)
        held = hosted_pointer.read_bytes()
        require(sha256(held) == HOSTED_SHA256, 'Hosted Pointer differs from qualified extraction binary')
        manifest['boot_consistency'] = verify_build_directory(build)
        (output/'pointer.bex').write_bytes(held)
        frozen = json.loads((FROZEN/'manifest.json').read_text())
        counter = (FROZEN/'counter.bex').read_bytes()
        require(sha256(counter) == frozen['files']['counter.bex']['sha256'], 'Frozen Counter hash mismatch')
        (output/'counter.bex').write_bytes(counter)
        build_app(ROOT/'examples/c/pointer.c', output/'pointer-window.bex', format='bex2',
                  window='native-v1', workspace_bytes=0, stack_bytes=16384)
        build_app(ROOT/'tests/native_window_document_app.c', output/'window-document.bex', format='bex2',
                  window='native-v1', workspace_bytes=0, stack_bytes=16384)
        apps = {name: (output/name).read_bytes() for name in
                ('counter.bex', 'pointer.bex', 'pointer-window.bex', 'window-document.bex')}
        for name, data in apps.items():
            manifest['apps'][name] = file_record(output/name)
            if data[:4] == b'BEX2':
                require(struct.unpack_from('<I', data, 12)[0] == 1, 'GUI app must require flag 1')
                manifest['apps'][name]['owned_pages'] = declared_pages(data)
        manifest['frozen_counter'] = frozen
        manifest['expected_document'] = dict(path=DOCUMENT, bytes=DOCUMENT_BYTES, sha256=sha256(document_bytes()))
        manifest['document_keys'] = {'s': 'versioned replace + owned async sync', 'a': 'sleep 2s then save while minimized',
            'v': 'versioned read and byte-for-byte verification', 'h': 'sleep 2s', 'm': 'public owned/free-page snapshot',
            'l': '64 numbered rows to exercise 48-row Output truncation', 'p': 'new explicitly published frame',
            'q/0/Escape': 'normal exit0', '7': 'normal exit7'}
        for profile in profiles:
            folder = output/profile; folder.mkdir()
            record = make_volume(folder/'data.img', profile, apps)
            manifest['profiles'][profile] = dict(ram_mib=PROFILES[profile], initial_volume=record, status='NOT RUN')
        if old_kernel:
            manifest['old_kernel'] = {name: file_record(old_kernel/name) for name in CORE_ARTIFACTS}
            manifest['old_kernel_directory'] = str(old_kernel.resolve())
            manifest['old_boot_consistency'] = verify_build_directory(old_kernel)
            manifest['old_refusal_volume'] = make_volume(output/'old-refusal.img', 'default', apps)
        else:
            manifest['lanes']['old-kernel-refusal'] = 'BLOCKED: explicit --old-kernel build directory required'
        manifest['status'] = 'PREPARED; NO GUEST RUN'
    except Exception as error:
        manifest['status'] = 'PREPARATION FAILED'; manifest['failure'] = repr(error); raise
    finally:
        save_json(output/'manifest.json', manifest)
    return manifest


def verify_inputs(manifest):
    for group in ('build', 'apps', 'source', 'old_kernel'):
        for name, record in manifest.get(group, {}).items():
            actual = file_record(record['path'])
            require(actual['bytes'] == record['bytes'] and actual['sha256'] == record['sha256'],
                    'Prepared input changed: '+group+'/'+name)


class Evidence:
    def __init__(self, output, session, phase):
        self.output, self.session, self.phase = output, session, phase
        self.font, self.shell = NativeFont(), ShellFont()
        self.report = dict(phase=phase, passed=False, guest_executed=True, screenshots=[], checks=[], states=[],
                           qemu_directory=str(session.directory), normal_shutdown=False)
        self.save()

    def save(self):
        save_json(self.output/(self.phase+'-results.json'), self.report)

    def frame(self, name):
        time.sleep(.25)
        path = self.output/(self.phase+'-'+name+'.png')
        self.session.command('screendump', {'filename': str(path), 'format': 'png'})
        self.report['screenshots'].append(file_record(path)); self.save()
        image = np.array(Image.open(path).convert('RGB'))
        require(image.shape == (720, 1280, 3), 'Collector requires normal1280x720 production display')
        return image

    def check(self, name, condition, **measured):
        self.report['checks'].append(dict(name=name, passed=bool(condition), measured=measured)); self.save()
        require(condition, name+': '+repr(measured))

    def pointer(self, name, side=None, logical=(160, 100)):
        self.session.move(1275, 670)  # Outside all sampled application pixels.
        image = self.frame(name); state = self.font.pointer(image, arranged_window(image, side), logical)
        self.report['states'].append(dict(name=name, **state)); self.save(); return state

    def document(self, name):
        self.session.move(1275, 670)
        image = self.frame(name); state = self.font.document(image, arranged_window(image))
        self.report['states'].append(dict(name=name, **state)); self.save(); return state

    def cycle(self, decoder, name):
        for attempt in range(8):
            self.session.key('alt-tab')
            try:
                return decoder(name+'-'+str(attempt))
            except AssertionError:
                continue
        raise AssertionError('Could not reveal '+name+' using ordinary Alt+Tab')

    def shutdown(self):
        shot = self.session.shutdown(self.output, self.phase)
        self.report['screenshots'].append(file_record(self.output/shot))
        self.report['normal_shutdown'] = True; self.report['passed'] = True; self.save()


def launch_files(session, name):
    session.launch('Files'); session.key('ctrl-f'); session.text('Programs'); session.key('down'); session.key('ret')
    session.key('ctrl-f'); session.text(name); session.key('down'); session.key('ret'); time.sleep(.4)


def toggle_output(session, window):
    session.click(window[0]+40, window[1]+48)


def pointer_stroke(session, evidence, state, label):
    before = state
    session.move(*point(state, 20, 60)); session.button(True)
    session.move(*point(state, 40, 70)); session.button(False)
    after = evidence.pointer(label, 'right', tuple(state['logical']))
    evidence.check(label+' commits once', after['done'] == before['done']+1 and after['buttons'] == after['drag'] == 0,
                   before=before, after=after)
    return after


def pointer_phase(session, evidence):
    session.boot(); session.origin(); session.move(1275, 670)
    launch_files(session, 'counter.bex'); session.key('spc')
    initial = evidence.font.counter(evidence.frame('frozen-counter-initial'))
    evidence.check('frozen Counter visibly paused', initial['paused'], counter=initial)
    session.key('s'); session.key('spc'); session.key('ctrl-m')
    session.launch('pointer.bex'); session.key('alt-left')
    hosted = evidence.font.locate(evidence.frame('qualified-hosted-pointer'))
    evidence.check('qualified hosted Pointer remains running', len(hosted) == 1 and hosted[0]['reset'] == 1, hosted=hosted)
    session.launch('pointer-window.bex'); session.key('alt-right')
    state = evidence.pointer('owned-initial', 'right')
    evidence.check('owned ADOPT initial reset', state['reset'] == 1 and state['buttons'] == state['drag'] == state['done'] == 0, state=state)
    state = pointer_stroke(session, evidence, state, 'owned-left-stroke')
    before = state; session.move(*point(state, 30, 65)); session.button(True); session.button(True, 'right')
    session.move(*point(state, 60, 75)); session.button(False); session.button(False, 'right')
    state = evidence.pointer('owned-chord', 'right')
    evidence.check('left/right chord commits once', state['done'] == before['done']+1 and not state['buttons'], state=state)
    # Capture crosses into the separately hosted peer without transferring
    # ownership. The screenshot cursor stays below both status text blocks.
    before = state; session.move(*point(state, 20, 60)); session.button(True)
    image = evidence.frame('capture-owner-down')
    hosted_before = evidence.font.locate(image)
    evidence.check('hosted peer remains fully visible beside owned capture', len(hosted_before) == 1, states=hosted_before)
    destination = (hosted_before[0]['origin'][0]+80*hosted_before[0]['scale'],
                   hosted_before[0]['origin'][1]+65*hosted_before[0]['scale'])
    session.move(*destination); image = evidence.frame('owned-capture-over-hosted-peer')
    captured = evidence.font.pointer(image, state['window'])
    hosted_after = evidence.font.locate(image)
    expected = [(destination[i]-state['origin'][i])*state['logical'][i]//state['viewport'][i] for i in (0, 1)]
    evidence.check('capture remains owned across peer window', captured['buttons'] == captured['drag'] == 1 and
        [captured['x'], captured['y']] == expected and len(hosted_after) == 1 and
        hosted_after[0]['sequence'] == hosted_before[0]['sequence'], captured=captured, peer=hosted_after, expected=expected)
    session.button(False); state = evidence.pointer('capture-peer-final-up', 'right')
    evidence.check('outside final UP commits owner once', state['done'] == before['done']+1 and not state['buttons'], state=state)
    before = state; session.move(*point(state, 20, 60)); session.button(True)
    session.key('ctrl-spc'); evidence.frame('launcher-overlay'); session.button(False); session.key('esc')
    state = evidence.pointer('launcher-dismissed', 'right')
    evidence.check('overlay cancels held preview', state['cancel'] > before['cancel'] and state['done'] == before['done'], state=state)
    before = state; session.key('o'); state = evidence.pointer('endpoint-reopened', 'right')
    evidence.check('reopen gives fresh reset', state['reset'] == before['reset']+1 and not state['buttons'], state=state)
    before = state; session.key('r'); state = evidence.pointer('logical-320', 'right', (320, 200))
    evidence.check('published resize changes geometry', state['geometry'] > before['geometry'], state=state)
    session.key('r'); state = evidence.pointer('logical-160', 'right')
    session.key('p'); toggle_output(session, state['window']); output = evidence.frame('pointer-output')
    try:
        evidence.font.pointer(output, state['window']); hidden = False
    except AssertionError:
        hidden = True
    evidence.check('Output replaces canvas', hidden)
    toggle_output(session, state['window']); state = evidence.pointer('output-dismissed', 'right')
    before = state; session.key('ctrl-m'); evidence.frame('owned-minimized')
    state = evidence.cycle(lambda name: evidence.pointer(name, 'right'), 'owned-restored')
    evidence.check('minimize/restore keeps clean buttons', not state['buttons'] and state['done'] == before['done'], state=state)
    session.key('q'); image = evidence.frame('successful-exit')
    try:
        evidence.font.pointer(image, arranged_window(image, 'right')); removed = False
    except AssertionError:
        removed = True
    evidence.check('normal exit0 removes owned canvas', removed)
    # The Terminal remains an ordinary visible shell across a GUI start.
    session.launch('Terminal'); session.text('echo shell marker'); session.key('ret')
    session.text('start /Programs/pointer-window.bex'); session.key('ret'); session.key('alt-ret')
    state = evidence.pointer('terminal-started-owned')
    evidence.check('Terminal start reaches owned backend', state['reset'] == 1, state=state)
    session.move(*point(state, 20, 60)); session.button(True); session.key('ctrl-w'); session.button(False)
    session.text('echo shell survives'); session.key('ret')
    image = evidence.frame('terminal-after-owned-close')
    evidence.check('calling shell accepts next command', evidence.shell.contains(image, (0, 36, image.shape[1], image.shape[0]-80), 'shell survives'))
    session.key('ctrl-w'); launch_files(session, 'pointer-window.bex'); session.key('alt-ret')
    state = evidence.pointer('files-started-owned'); evidence.check('Files launch reaches owned backend', state['reset'] == 1, state=state)
    session.key('ctrl-w')
    counter = evidence.cycle(lambda name: evidence.font.counter(evidence.frame(name)), 'counter-progress')
    session.key('spc'); counter = evidence.font.counter(evidence.frame('counter-final-paused')); session.key('s')
    evidence.check('frozen Counter progresses beside both backends', counter['paused'] and counter['value'] > initial['value'], initial=initial, final=counter)


def document_phase(session, evidence, manifest):
    session.boot(); session.origin(); session.move(1275, 670)
    session.launch('window-document.bex'); session.key('alt-ret'); baseline = evidence.document('document-initial')
    expected = manifest['apps']['window-document.bex']['owned_pages']
    evidence.check('public API reports declared owned pages', baseline['owned'] == expected, state=baseline, declared=expected)
    session.key('h'); sleeping = evidence.document('sleeping')
    evidence.check('sleep is visible before resuming', sleeping['state'] == 4, state=sleeping)
    time.sleep(2.2); awake = evidence.document('awake'); evidence.check('sleep resumes same instance', awake['slot'] == baseline['slot'] and awake['state'] == 1)
    session.launch('pointer-window.bex'); session.key('alt-ret'); evidence.pointer('page-peer-started'); session.key('ctrl-m')
    session.key('m'); peer_live = evidence.document('peer-pages-live')
    peer_pages = manifest['apps']['pointer-window.bex']['owned_pages']
    evidence.check('public free-page delta matches live GUI peer', baseline['free']-peer_live['free'] == peer_pages, before=baseline, after=peer_live)
    evidence.cycle(evidence.pointer, 'page-peer-restored'); session.key('ctrl-c'); stopped = evidence.pointer('stopped-frame')
    session.key('r'); inert = evidence.pointer('stopped-result-ignores-key')
    evidence.check('stopped result cannot resize or process keys', inert == stopped, before=stopped, after=inert)
    session.key('ctrl-m'); session.key('m'); after_stop = evidence.document('peer-pages-after-stop')
    evidence.check('Stop releases peer pages while result remains', after_stop['free'] == baseline['free'], before=baseline, after=after_stop)
    evidence.cycle(evidence.pointer, 'stopped-result-restored'); session.key('ctrl-w')
    session.key('a'); sleeping = evidence.document('save-after-sleep'); session.key('ctrl-m'); time.sleep(3.0)
    saved = evidence.cycle(evidence.document, 'saved-while-minimized')
    evidence.check('minimized app finishes owned durable sync', saved['saved'] == 1 and saved['result'] == 0, state=saved)
    session.key('v'); verified = evidence.document('live-byte-verification')
    evidence.check('public versioned read verifies exact saved bytes', verified['verified'] == 1, state=verified)
    session.key('l'); toggle_output(session, verified['window']); image = evidence.frame('output-newest')
    region = (verified['window'][0]+9, verified['window'][1]+71, verified['window'][2]-18, verified['window'][3]-80)
    evidence.check('bounded Output keeps newest row', evidence.shell.contains(image, region, 'WINDOW LOG ROW 63'))
    evidence.check('Output visibly reports discarded older rows', evidence.shell.matches(image,
        verified['window'][0]+90, verified['window'][1]+40, 'Older output discarded', 'ui'))
    for _ in range(4): session.key('pgup')
    image = evidence.frame('output-oldest-retained')
    evidence.check('48-row Output starts at row16', evidence.shell.contains(image, region, 'WINDOW LOG ROW 16') and
                   not evidence.shell.contains(image, region, 'WINDOW LOG ROW 15'))
    toggle_output(session, verified['window']); session.key('7'); image = evidence.frame('normal-exit-seven')
    ended = evidence.font.document(image, arranged_window(image))
    evidence.check('nonzero ordinary exit retains inert final canvas', ended['verified'] == 1, state=ended)
    toggle_output(session, ended['window']); image = evidence.frame('exit-seven-result-output')
    evidence.check('nonzero result exposes exact APP reason and value', evidence.shell.contains(image, region, 'APP (1), value 7.'))
    session.key('ctrl-w'); session.launch('window-document.bex'); session.key('alt-ret'); reused = evidence.document('reused-slot')
    evidence.check('relaunch resets state and reclaims prior pages', reused['saved'] == 0 and reused['verified'] == 0 and
        reused['phase'] == 0 and reused['free'] == baseline['free'], state=reused, baseline=baseline)
    session.key('q'); evidence.frame('document-success-closed')


def stopped_volume(disk, manifest, require_document=True):
    slot, generation, nodes = load(disk.read_bytes())
    for name, record in manifest['apps'].items():
        actual = nodes[resolve(nodes, '/Programs/'+name)]['data']
        require(sha256(actual) == record['sha256'], 'Guest changed executable '+name)
    result = dict(slot=slot, generation=generation, volume=file_record(disk))
    if require_document:
        data = nodes[resolve(nodes, DOCUMENT)]['data']
        require(data == document_bytes(), 'Durable document bytes do not match deterministic fixture')
        result['document'] = dict(bytes=len(data), sha256=sha256(data))
    return result


def phase_run(build, disk, ram, folder, phase, action, manifest):
    verify_inputs(manifest)
    # Boot the exact held production image, copied byte-for-byte into the
    # evidence folder. Never reconstruct, patch, or open the frozen source for
    # writing. The disposable boot copy must remain unchanged through the run.
    boot_image = folder/(phase+'-boot.img')
    require(not boot_image.exists(), 'Refusing to reuse phase boot image')
    shutil.copyfile(build/'baseos.img', boot_image)
    boot_record = file_record(boot_image)
    evidence = None; session = None
    try:
        with Session(build, 'native-window-'+phase, extra=('-m', str(ram)+'M', '-drive', f'file={disk},format=raw,index=0,if=ide'), image=boot_image) as session:
            evidence = Evidence(folder, session, phase)
            evidence.report['boot_image'] = boot_record; evidence.save()
            action(session, evidence)
            serial = session.log.read_text(); evidence.check('normal production serial markers',
                'DESKTOP-READY\n' in serial and 'PANIC:' not in serial)
            evidence.shutdown()
        require(session.process.poll() == 0, 'Normal guest shutdown did not complete')
        require(file_record(boot_image)['sha256'] == boot_record['sha256'], 'Guest changed disposable production boot image')
        verify_inputs(manifest)
        return evidence.report
    except Exception as error:
        if evidence:
            evidence.report['passed'] = False; evidence.report['failure'] = repr(error); evidence.save()
        raise
    finally:
        if session:
            # Context manager has closed QEMU before copying serial/stderr or
            # any caller is permitted to inspect the volume.
            for name in ('serial.log', 'stderr.log'):
                source = session.directory/name
                if source.exists():
                    target = folder/(phase+'-'+name); shutil.copyfile(source, target)
                    if evidence:
                        evidence.report.setdefault('logs', {})[name] = file_record(target); evidence.save()
            try:
                verify_inputs(manifest)
            except Exception as changed:
                if evidence:
                    evidence.report['passed'] = False; evidence.report['input_verification_failure'] = repr(changed); evidence.save()
                raise



def run(output):
    manifest = json.loads((output/'manifest.json').read_text()); verify_inputs(manifest)
    require(manifest['status'] == 'PREPARED; NO GUEST RUN', 'Run needs a fresh prepared evidence directory')
    require(not manifest['source_dirty'], 'Guest run requires a clean frozen source preparation')
    require(manifest.get('build_info', {}).get('dirty') is False, 'Guest run requires clean production build metadata')
    manifest['status'] = 'RUNNING'; manifest['preparation_only'] = False; save_json(output/'manifest.json', manifest)
    build = Path(manifest['build_directory'])
    try:
        for profile, setup in manifest['profiles'].items():
            folder = output/profile; disk = Path(setup['initial_volume']['path']); ram = setup['ram_mib']
            require(file_record(disk)['sha256'] == setup['initial_volume']['sha256'], 'Prepared volume changed before boot')
            phase_run(build, disk, ram, folder, 'pointer', pointer_phase, manifest); verify_inputs(manifest)
            setup['after_pointer'] = stopped_volume(disk, manifest, False)
            phase_run(build, disk, ram, folder, 'document', lambda s, e: document_phase(s, e, manifest), manifest); verify_inputs(manifest)
            setup['after_document'] = stopped_volume(disk, manifest)
            def reboot(s, e):
                s.boot(); s.origin(); s.move(1275, 670); s.launch('window-document.bex'); s.key('alt-ret'); s.key('v')
                state = e.document('untouched-cold-reboot-read')
                e.check('cold reboot reads exact saved bytes through public API', state['verified'] == 1 and state['saved'] == 0, state=state)
                s.key('q')
            # No data-volume mutation by this collector occurs between shutdown
            # and cold boot. The production startup may update ordinary prefs.
            phase_run(build, disk, ram, folder, 'reboot', reboot, manifest); verify_inputs(manifest)
            setup['after_reboot'] = stopped_volume(disk, manifest); setup['status'] = 'PASSED IMPLEMENTED LANES'
            save_json(output/'manifest.json', manifest)
        for lane in LANES[:-1]: manifest['lanes'][lane] = 'PASSED IMPLEMENTED CHECKS'
        if manifest.get('old_kernel_directory'):
            old = Path(manifest['old_kernel_directory']); disk = output/'old-refusal.img'
            require(file_record(disk)['sha256'] == manifest['old_refusal_volume']['sha256'], 'Old-refusal fixture changed before boot')
            def refusal(s, e):
                s.boot(); s.origin(); s.move(1275, 670); s.launch('pointer-window.bex'); s.key('alt-ret')
                image = e.frame('required-gui-refused')
                e.check('preceding kernel visibly refuses required GUI executable', e.shell.contains(image,
                    (0, 36, image.shape[1], image.shape[0]-80), 'format or ABI is not enabled.'))
            phase_run(old, disk, 64, output, 'old-refusal', refusal, manifest); verify_inputs(manifest)
            manifest['lanes']['old-kernel-refusal'] = 'PASSED VISIBLE REFUSAL; NO PROCESS/PAGE COUNTER CLAIM'
        manifest['status'] = 'IMPLEMENTED COLLECTOR LANES PASSED; SEPARATE GATES REMAIN'
    except Exception as error:
        manifest['status'] = 'FAILED; ALL EVIDENCE RETAINED'; manifest['failure'] = repr(error); raise
    finally:
        save_json(output/'manifest.json', manifest)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', nargs='?', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--hosted-pointer', type=Path)
    parser.add_argument('--old-kernel', type=Path, help='explicit held preceding production build directory')
    parser.add_argument('--profile', choices=(*PROFILES, 'both'), default='both')
    parser.add_argument('--run', action='store_true', help='execute prepared collector ONLY AFTER an explicit QEMU grant')
    args = parser.parse_args()
    if args.run:
        require(args.build is None and args.hosted_pointer is None and args.old_kernel is None,
                'Run consumes only existing manifest; prepare new inputs separately')
        report = run(args.output.resolve())
    else:
        require(args.build is not None, 'Preparation requires a production build directory')
        profiles = tuple(PROFILES) if args.profile == 'both' else (args.profile,)
        report = prepare(args.build.resolve(), args.output.resolve(),
                         (args.hosted_pointer or args.build/'pointer.bex').resolve(), profiles,
                         args.old_kernel.resolve() if args.old_kernel else None)
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
