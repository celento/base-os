"""Deterministic production PS/2 gate for the real SDK Pointer example.

Run only when a QEMU slot is available. Every running-guest assertion comes from
ordinary screenshots decoded with the example's original 3x5 font. Input uses
normal QMP PS/2 keyboard/mouse events; only serial boot markers are inspected.
Guest memory, debuggers, injected kernel callbacks, deliberate faults, fuzzing,
and live-volume reads are forbidden. Each run builds both real example formats,
uses a fresh disposable volume, and normally exits through System -> Shutdown.

python3 tools/native_pointer_input_test.py BUILD --profile default --output DIR
python3 tools/native_pointer_input_test.py BUILD --profile large --output DIR
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image

from build_app import build as build_app
from files_view_input_test import FilesSession
from init_data import initialize
from paint_save_input_test import PaintSession
from volume import data_layout, encode_snapshot, load, resolve

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / 'examples/c/pointer.c'
FROZEN = ROOT / 'tests/fixtures/bex1-hour05'
COLORS = {0: (0, 0, 0), 1: (60, 60, 60), 6: (242, 201, 76),
          7: (63, 163, 91), 8: (42, 167, 200)}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Font:
    """Exact source-derived bitmap decoder, never OCR or guessed characters."""
    def __init__(self, source=EXAMPLE):
        text = source.read_text()
        match = re.search(r'\bglyphs\[36\]\[5\]\s*=\s*(\{.*?\});', text, re.S)
        require(match is not None, 'Pointer glyph table was not found')
        rows = ast.literal_eval(match[1].replace('{', '[').replace('}', ']'))
        require(len(rows) == 36 and all(len(row) == 5 for row in rows), 'Unexpected glyph table')
        self.rows = dict(zip('0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ', rows))
        self.rows.update({' ': [0]*5, '-': [0, 0, 7, 0, 0],
                          ':': [0, 2, 0, 2, 0], '=': [0, 7, 0, 7, 0],
                          '/': [1, 1, 2, 4, 4]})
        self.masks = {char: np.array([[(bits >> (2-x)) & 1 for x in range(3)]
                                     for bits in value], dtype=bool)
                      for char, value in self.rows.items()}
        self.characters = {mask.tobytes(): char for char, mask in self.masks.items()}
        require(len(self.characters) == len(self.masks), 'Ambiguous source glyphs')

    def bitmap(self, text):
        result = np.zeros((5, len(text)*4), dtype=bool)
        for index, char in enumerate(text):
            result[:, index*4:index*4+3] = self.masks[char]
        return result

    def line(self, image, origin, scale, y, color, count):
        x0, y0 = origin
        chars = []
        for index in range(count):
            x = x0 + (2+4*index)*scale
            top = y0 + y*scale
            cell = image[top:top+5*scale, x:x+4*scale]
            require(cell.shape == (5*scale, 4*scale, 3), 'Status row is clipped')
            small = cell[::scale, ::scale]
            require(np.array_equal(cell, small.repeat(scale, 0).repeat(scale, 1)),
                    'Status glyph is occluded or not integer-scaled')
            on = np.all(small == COLORS[color], axis=2)
            off = np.all(small == COLORS[0], axis=2)
            require(np.all(on | off) and not np.any(on[:, 3]), 'Status glyph has unexpected pixels')
            char = self.characters.get(on[:, :3].tobytes())
            require(char is not None, 'Unknown exact source glyph')
            chars.append(char)
        return ''.join(chars).rstrip()

    def decode(self, image, origin, scale):
        # All status rows fit in the minimum 160-pixel logical canvas.
        rows = [self.line(image, origin, scale, y, color, 38)
                for y, color in ((2, 8), (9, 7), (16, 7), (23, 7), (30, 6), (37, 8))]
        expressions = (r'POINTER (\d+)X(\d+)', r'S=([0-9A-F]{8}):([0-9A-F]{8})',
                       r'X=(-?\d+) Y=(-?\d+)', r'B=(\d+) G=(\d+) T=(\d+)',
                       r'RESET=(\d+) CANCEL=(\d+)', r'DRAG=(\d+) DONE=(\d+)')
        matches = [re.fullmatch(expression, row) for expression, row in zip(expressions, rows)]
        require(all(matches), 'Unexpected Pointer status rows: ' + repr(rows))
        values = [match.groups() for match in matches]
        width, height = map(int, values[0])
        return dict(origin=list(origin), scale=scale, logical=[width, height],
                    viewport=[width*scale, height*scale], rows=rows,
                    sequence=int(values[1][0], 16)*2**32 + int(values[1][1], 16),
                    x=int(values[2][0]), y=int(values[2][1]), buttons=int(values[3][0]),
                    geometry=int(values[3][1]), stream=int(values[3][2]),
                    reset=int(values[4][0]), cancel=int(values[4][1]),
                    drag=int(values[5][0]), done=int(values[5][1]))

    def locate(self, image):
        cyan = np.all(image == COLORS[8], axis=2)
        # The top-left P pixel starts a cyan component at the label origin.
        starts = cyan.copy()
        starts[:, 1:] &= ~cyan[:, :-1]
        starts[1:] &= ~cyan[:-1]
        ys, xs = np.nonzero(starts)
        found = {}
        for scale in (1, 2, 4):
            template = self.bitmap('POINTER').repeat(scale, 0).repeat(scale, 1)
            height, width = template.shape
            for y, x in zip(ys.tolist(), xs.tolist()):
                origin = (x-2*scale, y-2*scale)
                if min(origin) < 0 or y+height > image.shape[0] or x+width > image.shape[1]:
                    continue
                if not np.array_equal(cyan[y:y+height, x:x+width], template):
                    continue
                try:
                    state = self.decode(image, origin, scale)
                except AssertionError:
                    continue  # Partial/occluded labels are not decoded states.
                found[origin] = state
        return sorted(found.values(), key=lambda state: state['origin'])

    def counter(self, image):
        """Decode the unchanged frozen Counter's four large visible digits."""
        # The reference Counter source uses these same ten original digit masks.
        for color in (7, 6):  # Running and paused stripe colors.
            mask = np.all(image == COLORS[color], axis=2)
            starts = mask.copy()
            starts[:, 1:] &= ~mask[:, :-1]
            starts[1:] &= ~mask[:-1]
            ys, xs = np.nonzero(starts)
            for scale in (1, 2, 4):
                for y, x in zip(ys.tolist(), xs.tolist()):
                    if x < 4*scale or y < 4*scale:
                        continue
                    if not np.all(mask[y:y+4*scale, x:x+152*scale]) or x+152*scale > image.shape[1]:
                        continue
                    ox, oy = x-4*scale, y-4*scale
                    digits = []
                    for index in range(4):
                        bits = np.zeros((5, 3), dtype=bool)
                        valid = True
                        for row in range(5):
                            for col in range(3):
                                px = ox+(12+index*37+col*8)*scale
                                py = oy+(22+row*10)*scale
                                block = image[py:py+9*scale, px:px+7*scale]
                                if block.shape != (9*scale, 7*scale, 3):
                                    valid = False
                                    continue
                                black = np.all(block == COLORS[0])
                                ink = np.all(block == COLORS[6]) or np.all(block == COLORS[8])
                                valid &= bool(black or ink)
                                bits[row, col] = ink
                        char = self.characters.get(bits.tobytes())
                        if not valid or char is None or char not in '0123456789':
                            break
                        digits.append(char)
                    if len(digits) == 4:
                        return dict(origin=[ox, oy], scale=scale, value=int(''.join(digits)),
                                    paused=color == 6, digits=''.join(digits))
        raise AssertionError('A complete unobscured frozen Counter canvas was not found')


class Session(PaintSession):
    origin = FilesSession.origin
    move = FilesSession.move

    def button(self, down, button='left', delay=.15):
        require(button in ('left', 'right'), 'Only ordinary pointer buttons are allowed')
        self.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': button, 'down': down}}]})
        time.sleep(delay)

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        self.button(False)

    def release_motion(self, dx, dy):
        require(abs(dx) <= 80 and abs(dy) <= 80, 'Final-UP motion must fit an ordinary PS/2 packet')
        events = [{'type': 'rel', 'data': {'axis': axis, 'value': value}}
                  for axis, value in (('x', dx), ('y', dy)) if value]
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': False}})
        self.command('input-send-event', {'events': events})
        self.pointer = self.pointer[0]+dx, self.pointer[1]+dy
        time.sleep(.25)

    def wheel(self, button):
        require(button in ('wheel-up', 'wheel-down'), 'Expected ordinary wheel button')
        self.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': button, 'down': True}},
            {'type': 'btn', 'data': {'button': button, 'down': False}}]})
        time.sleep(.25)


class Evidence:
    def __init__(self, output, report, font):
        self.output, self.report, self.font = output, report, font
        self.session = None

    def save(self):
        (self.output / 'results.json').write_text(json.dumps(self.report, indent=2)+'\n')
        (self.output / 'decoded-states.json').write_text(json.dumps(self.report['states'], indent=2)+'\n')

    def frame(self, name):
        time.sleep(.25)
        path = self.output / (name+'.png')
        self.session.command('screendump', {'filename': str(path), 'format': 'png'})
        image = np.array(Image.open(path).convert('RGB'))
        self.report['screenshots'].append(dict(name=name, file=path.name, sha256=sha256(path.read_bytes())))
        self.save()
        return image

    def state(self, name, origin=None):
        image = self.frame(name)
        states = self.font.locate(image)
        self.report['states'].append(dict(name=name, canvases=states))
        self.save()
        matches = [state for state in states if origin is None or state['origin'] == list(origin)]
        require(len(matches) == 1, f'{name}: expected one complete canvas at {origin}; found {states}')
        return matches[0], image

    def pair(self, name, first, second):
        image = self.frame(name)
        states = self.font.locate(image)
        self.report['states'].append(dict(name=name, canvases=states))
        self.save()
        result = []
        for origin in (first, second):
            matches = [state for state in states if state['origin'] == list(origin)]
            require(len(matches) == 1, f'{name}: peer canvas at {origin} is not fully visible')
            result.append(matches[0])
        return result

    def check(self, name, condition, **measured):
        entry = dict(name=name, passed=bool(condition), measured=measured)
        self.report['checks'].append(entry)
        self.save()
        require(condition, name+': '+repr(measured))


def point(state, x, y):
    return state['origin'][0]+x*state['scale'], state['origin'][1]+y*state['scale']


def ink_at(image, state, x, y, color):
    px, py = point(state, x, y)
    scale = state['scale']
    return bool(np.all(image[py:py+scale, px:px+scale] == COLORS[color]))


def fixture(output, profile):
    build_app(EXAMPLE, output / 'pointer.bex')
    build_app(EXAMPLE, output / 'pointer2.bex', format='bex2')
    frozen = (FROZEN / 'counter.bex').read_bytes()
    manifest = json.loads((FROZEN / 'manifest.json').read_text())
    require(sha256(frozen) == manifest['files']['counter.bex']['sha256'], 'Frozen Counter hash mismatch')
    (output / 'counter.bex').write_bytes(frozen)
    (output / 'frozen-manifest.json').write_bytes((FROZEN / 'manifest.json').read_bytes())
    apps = {name: (output / name).read_bytes() for name in ('pointer.bex', 'pointer2.bex', 'counter.bex')}
    for name, magic in (('pointer.bex', b'BEX1'), ('pointer2.bex', b'BEX2'), ('counter.bex', b'BEX1')):
        require(apps[name][:4] == magic, 'Unexpected application format for '+name)
    disk = output / 'pointer-data.img'
    require(initialize(disk, profile=profile), 'Refusing to reuse a volume fixture')
    nodes = {0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
             1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0),
             2: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0)}
    for name, data in apps.items():
        nodes[len(nodes)] = dict(parent=1, name=name, directory=0, app=0, data=data, modified=1)
    baselines = {}
    for slot in range(1, 9):
        name = f'counter-{slot}.txt'
        baselines['/Documents/'+name] = 100+slot
        nodes[len(nodes)] = dict(parent=2, name=name, directory=0, app=0,
                                data=f'{100+slot}\n'.encode(), modified=1)
    layout = data_layout(profile)
    header, payload = encode_snapshot(nodes, layout, 1)
    with disk.open('r+b') as stream:
        stream.seek(layout.lbas[0]*512)
        stream.write(header.ljust(512, b'\0'))
        stream.write(payload)
    return disk, apps, baselines, manifest


def stroke(session, evidence, state, name, start=(20, 60), end=(40, 70)):
    session.move(*point(state, *start)); session.button(True)
    held, _ = evidence.state(name+'-down', state['origin'])
    evidence.check(name+' accepts ordinary DOWN', held['buttons'] == 1 and held['drag'] == 1,
                   before=state, after=held)
    session.move(*point(state, *end)); session.button(False)
    after, image = evidence.state(name+'-up', state['origin'])
    evidence.check(name+' commits on final UP', after['done'] == state['done']+1 and
                   after['buttons'] == 0 and after['drag'] == 0 and
                   (after['x'], after['y']) == end and ink_at(image, after, *start, 8),
                   before=state, after=after, cyan_start_pixel=True if ink_at(image, after, *start, 8) else False)
    return after


def focus_counter(session, evidence, name):
    # Cycle only the known live windows. A partially covered canvas is rejected.
    for attempt in range(5):
        session.key('alt-tab')
        image = evidence.frame(f'{name}-cycle-{attempt+1}')
        try:
            state = evidence.font.counter(image)
            evidence.report['states'].append(dict(name=name, counter=state))
            evidence.save()
            return state
        except AssertionError:
            pass
    raise AssertionError('Could not reveal the frozen Counter through ordinary window cycling')


def exercise(session, evidence):
    font = evidence.font
    session.boot(); session.origin(); session.move(1100, 620)
    serial = session.log.read_text()
    markers = ['Kernel started\n', 'FS loaded from disk\n', 'FS ready\n', 'DESKTOP-READY\n']
    evidence.check('production boot serial markers', all(marker in serial for marker in markers), markers=markers)
    session.launch('counter.bex'); session.key('spc')
    counter_initial = font.counter(evidence.frame('counter-initial-paused'))
    evidence.check('frozen Counter starts and pauses visibly', counter_initial['paused'], counter=counter_initial)
    session.key('s'); evidence.frame('counter-initial-saved')
    session.key('spc'); session.key('ctrl-m')

    session.launch('pointer.bex')
    state, _ = evidence.state('bex1-initial')
    evidence.check('BEX1 initial OPEN reset and default viewport', state['logical'] == [160, 100] and
                   state['scale'] == 2 and state['reset'] == 1 and state['stream'] == 1 and
                   state['buttons'] == state['drag'] == state['done'] == 0,
                   state=state)
    origin = state['origin']
    session.move(*point(state, 20, 60)); session.button(True)
    held, _ = evidence.state('simple-left-down', origin)
    evidence.check('left DOWN starts capture gesture', held['buttons'] == held['drag'] == 1 and held['done'] == 0,
                   state=held)
    session.move(*point(state, 40, 70)); session.release_motion(4*state['scale'], 3*state['scale'])
    state, image = evidence.state('simple-final-up-motion', origin)
    evidence.check('committed left stroke including final-UP motion', state['done'] == 1 and
                   state['buttons'] == state['drag'] == 0 and (state['x'], state['y']) == (44, 73) and
                   ink_at(image, state, 20, 60, 8), state=state)

    session.key('c'); before, _ = evidence.state('chord-cleared', origin)
    session.move(*point(before, 20, 55)); session.button(True); session.button(True, 'right')
    chord, _ = evidence.state('left-right-chord', origin)
    evidence.check('left and right are a chord', chord['buttons'] == 3 and chord['drag'] == 1 and
                   chord['done'] == before['done'], state=chord)
    session.move(*point(before, 40, 65)); session.button(False)
    right, _ = evidence.state('chord-right-remains', origin)
    evidence.check('releasing left retains right capture', right['buttons'] == 2 and right['drag'] == 1 and
                   right['done'] == before['done'], state=right)
    session.button(False, 'right'); state, image = evidence.state('chord-final-up', origin)
    evidence.check('chord commits once on last UP', state['done'] == before['done']+1 and
                   state['buttons'] == state['drag'] == 0 and ink_at(image, state, 30, 60, 7), state=state)

    session.key('c'); before, _ = evidence.state('outside-cleared', origin)
    session.move(*point(before, 30, 70)); session.button(True)
    outside = (origin[0]-19, origin[1]-51)
    require(min(outside) >= 0, 'Window position cannot support the chosen outside capture check')
    session.move(*outside)
    captured, _ = evidence.state('capture-negative-outside-window', origin)
    expected = (-19//before['scale'], -51//before['scale'])
    evidence.check('capture reports signed coordinates outside its host window',
                   (captured['x'], captured['y']) == expected and captured['buttons'] == captured['drag'] == 1,
                   screen=list(outside), expected=list(expected), state=captured)
    session.button(False); state, _ = evidence.state('capture-outside-final-up', origin)
    evidence.check('outside final UP clears gesture and commits once', state['done'] == before['done']+1 and
                   state['buttons'] == state['drag'] == 0 and (state['x'], state['y']) == expected, state=state)

    session.key('c'); before, _ = evidence.state('launcher-cleared', origin)
    session.move(*point(before, 20, 60)); session.button(True); session.move(*point(before, 40, 70))
    session.key('ctrl-spc'); evidence.frame('launcher-open-while-held')
    session.move(1100, 620); session.button(False); session.key('esc')
    state, image = evidence.state('launcher-cancelled', origin)
    evidence.check('launcher cancellation discards held preview without commit', state['cancel'] > before['cancel'] and
                   state['done'] == before['done'] and state['buttons'] == state['drag'] == 0 and
                   ink_at(image, state, 30, 65, 0), before=before, after=state)

    session.move(*point(state, 90, 80)); before, _ = evidence.state('wheel-before', origin)
    for wheel in ('wheel-down', 'wheel-up'):
        session.wheel(wheel); after, _ = evidence.state(wheel, origin)
        evidence.check(wheel+' advances visible event sequence without buttons',
                       after['sequence'] > before['sequence'] and after['buttons'] == after['drag'] == 0 and
                       after['done'] == before['done'] and (after['x'], after['y']) == (90, 80),
                       before=before, after=after)
        before = after

    session.move(*point(before, 30, 60)); session.button(True); session.key('r')
    state, _ = evidence.state('r-published-while-held', origin)
    evidence.check('R publishes 320x200 geometry and cancels held gesture', state['logical'] == [320, 200] and
                   state['scale'] == 1 and state['geometry'] > before['geometry'] and
                   state['cancel'] > before['cancel'] and state['done'] == before['done'] and
                   state['buttons'] == state['drag'] == 0, before=before, after=state)
    session.move(*point(state, 80, 150))
    suppressed, _ = evidence.state('r-held-tail-suppressed', origin)
    evidence.check('resize does not reinterpret a held tail as new DOWN',
                   suppressed['buttons'] == suppressed['drag'] == 0 and suppressed['done'] == state['done'], state=suppressed)
    session.button(False)
    state = stroke(session, evidence, state, 'resized-fresh-stroke', (80, 150), (110, 160))
    before = state; session.key('alt-ret'); state, _ = evidence.state('maximized')
    evidence.check('maximized published canvas uses 2x320 viewport', state['logical'] == [320, 200] and
                   state['scale'] == 2 and state['geometry'] > before['geometry'], before=before, after=state)
    session.move(*point(state, 250, 150)); state, _ = evidence.state('maximized-coordinate-mapping', state['origin'])
    evidence.check('maximized hover maps exact logical coordinates', (state['x'], state['y']) == (250, 150) and
                   state['buttons'] == 0, state=state)
    session.key('alt-ret'); session.key('r'); state, _ = evidence.state('default-geometry-restored', origin)
    evidence.check('restored 160x100 published geometry', state['logical'] == [160, 100] and state['scale'] == 2, state=state)

    session.key('c'); before, _ = evidence.state('reopen-cleared', origin)
    session.move(*point(before, 30, 60)); session.button(True); session.key('o')
    state, _ = evidence.state('o-reopened-while-held', origin)
    evidence.check('O gives a fresh OPEN reset without committing old gesture', state['reset'] == before['reset']+1 and
                   state['stream'] == 1 and state['geometry'] == 1 and state['sequence'] < before['sequence'] and
                   state['buttons'] == state['drag'] == 0 and state['done'] == before['done'], before=before, after=state)
    session.move(*point(state, 50, 70)); held, _ = evidence.state('reopen-held-tail-suppressed', origin)
    evidence.check('reopen suppresses old held buttons', held['buttons'] == held['drag'] == 0 and
                   held['done'] == before['done'], state=held)
    session.button(False); state = stroke(session, evidence, state, 'reopen-fresh-stroke')

    before = state; session.move(*point(state, 30, 60)); session.button(True); session.key('ctrl-m')
    hidden = evidence.frame('minimized-while-held')
    evidence.check('minimize removes the visible Pointer canvas', not font.locate(hidden))
    session.move(1100, 620); session.button(False)
    # Counter is the only other window; both may currently be minimized.
    for attempt in range(3):
        session.key('alt-tab'); image = evidence.frame(f'restore-cycle-{attempt+1}')
        if any(item['origin'] == origin for item in font.locate(image)):
            break
    state, _ = evidence.state('restored-after-minimize', origin)
    evidence.check('restoring minimized capture has no sticky button or commit', state['cancel'] > before['cancel'] and
                   state['buttons'] == state['drag'] == 0 and state['done'] == before['done'], before=before, after=state)

    session.move(*point(state, 30, 60)); session.button(True); session.key('ctrl-w')
    image = evidence.frame('forced-close-while-held')
    evidence.check('forced close removes Pointer canvas', not font.locate(image))
    session.move(1100, 620); session.button(False); session.launch('pointer.bex')
    state, _ = evidence.state('relaunch-after-forced-close')
    evidence.check('relaunch has fresh reset and no sticky gesture', state['reset'] == 1 and
                   state['buttons'] == state['drag'] == state['done'] == 0, state=state)
    state = stroke(session, evidence, state, 'relaunch-fresh-stroke')

    # A normal left snap leaves exactly enough room for a second ordinary
    # 640-pixel Terminal. Its title drag is derived from its decoded canvas:
    # window origin = canvas origin - (13,45), not private window state.
    session.key('alt-left'); first, _ = evidence.state('peer-a-left-positioned')
    session.launch('pointer2.bex'); second, image = evidence.state('bex2-initial')
    evidence.check('BEX2 initial OPEN reset', second['reset'] == 1 and second['buttons'] == second['drag'] == second['done'] == 0,
                   state=second)
    wx, wy = second['origin'][0]-13, second['origin'][1]-45
    desired_x, desired_y = image.shape[1]-642, 240
    title = (wx+300, wy+16)
    session.move(*title); session.button(True)
    session.move(title[0]+desired_x-wx, title[1]+desired_y-wy); session.button(False)
    second, _ = evidence.state('peer-b-title-dragged', (desired_x+13, desired_y+45))
    session.click(first['origin'][0]+150, first['origin'][1]-29)
    first, second = evidence.pair('independent-peer-baseline', first['origin'], second['origin'])
    evidence.check('two independent positioned peer canvases are simultaneously decoded', first['origin'] != second['origin'] and
                   first['logical'] == second['logical'] == [160, 100] and second['done'] == 0,
                   first=first, second=second)
    session.move(*point(first, 20, 60)); session.button(True); destination = point(second, 50, 60)
    session.move(*destination)
    captured, untouched = evidence.pair('capture-over-peer', first['origin'], second['origin'])
    expected = tuple((destination[i]-first['origin'][i])//first['scale'] for i in (0, 1))
    evidence.check('first peer keeps capture over second canvas', (captured['x'], captured['y']) == expected and
                   captured['buttons'] == captured['drag'] == 1 and untouched['sequence'] == second['sequence'] and
                   untouched['buttons'] == untouched['drag'] == untouched['done'] == 0,
                   captured=captured, untouched=untouched, expected=list(expected))
    session.button(False)
    first_after, second_after = evidence.pair('capture-peer-final-up', first['origin'], second['origin'])
    evidence.check('UP over peer commits only capture owner', first_after['done'] == first['done']+1 and
                   first_after['buttons'] == first_after['drag'] == 0 and
                   second_after['done'] == second['done'] and second_after['sequence'] == second['sequence'],
                   first=first_after, second=second_after)
    second_after = stroke(session, evidence, second_after, 'bex2-independent-stroke')
    peers = evidence.pair('peer-isolated-commits', first['origin'], second['origin'])
    evidence.check('peer strokes remain instance-local', peers[0]['done'] == first_after['done'] and
                   peers[1]['done'] == second['done']+1, first=peers[0], second=peers[1])
    session.key('ctrl-w')  # Close focused BEX2 peer, leaving BEX1 and Counter.

    session.move(1100, 620); focus_counter(session, evidence, 'counter-before-idle')
    session.key('spc'); before_counter = font.counter(evidence.frame('counter-before-idle-paused'))
    require(before_counter['paused'], 'Counter did not pause before the idle interval')
    session.key('s'); session.key('spc'); session.key('ctrl-m')
    session.click(first['origin'][0]+150, first['origin'][1]-29)
    before, _ = evidence.state('pointer-idle-begin', first['origin'])
    began = time.monotonic(); time.sleep(3.2)
    after, _ = evidence.state('pointer-idle-end', first['origin'])
    idle_seconds = time.monotonic()-began
    evidence.check('Pointer status remains stable during idle wait interval', before == after,
                   seconds=idle_seconds, before=before, after=after)
    focus_counter(session, evidence, 'counter-after-idle'); session.key('spc')
    counter_final = font.counter(evidence.frame('counter-after-idle-paused'))
    evidence.check('unchanged frozen Counter progresses beside idle Pointer', counter_final['paused'] and
                   counter_final['value'] > before_counter['value'], before=before_counter,
                   after=counter_final, idle_wall_seconds=idle_seconds)
    session.key('s'); evidence.frame('counter-final-saved')
    evidence.report['counter'] = dict(initial=counter_initial, before_idle=before_counter, final=counter_final)
    evidence.save()


def run(build, profile, output):
    output.mkdir(parents=True, exist_ok=False)
    report = dict(profile=profile, build=str(build), output=str(output), passed=False,
                  observation_policy='PS/2 input, screenshots, serial boot markers, stopped-volume reads only',
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  checks=[], screenshots=[], states=[], normal_shutdown=False)
    evidence = Evidence(output, report, Font())
    session = None
    try:
        disk, apps, baselines, manifest = fixture(output, profile)
        report['kernel_build'] = json.loads((build / 'build_info.json').read_text())
        report['hashes'] = {name: sha256((build / name).read_bytes())
                            for name in ('kernel.bin', 'kernel.elf', 'boot.bin')}
        report['hashes'].update({name: sha256(data) for name, data in apps.items()})
        report['hashes'].update({'pointer.c': sha256(EXAMPLE.read_bytes()),
                                'baseos.h': sha256((ROOT / 'sdk/baseos.h').read_bytes()),
                                'baseos_abi.h': sha256((ROOT / 'sdk/baseos_abi.h').read_bytes()),
                                'native_pointer_input_test.py': sha256(Path(__file__).read_bytes())})
        report['frozen_counter_provenance'] = manifest
        report['counter_seed_values'] = baselines
        evidence.save()
        extra = ['-drive', f'file={disk},format=raw,index=0,if=ide']
        if profile == 'large':
            extra += ['-m', '128M']
        with Session(build, 'native-pointer-'+profile, extra=tuple(extra)) as session:
            evidence.session = session
            report['qemu_evidence_directory'] = str(session.directory)
            print('Pointer input evidence:', output, 'QEMU:', session.directory, flush=True)
            exercise(session, evidence)
            report['screenshots'].append(dict(file=session.shutdown(output, 'final'), name='normal-system-shutdown'))
            report['normal_shutdown'] = True
        # No disk bytes are read while QEMU is alive.
        require(session.process.poll() == 0, 'QEMU did not exit through normal shutdown')
        raw = disk.read_bytes()
        slot, generation, nodes = load(raw)
        for name, expected in apps.items():
            require(nodes[resolve(nodes, '/Programs/'+name)]['data'] == expected,
                    'Guest application bytes changed: '+name)
        saved = {path: int(nodes[resolve(nodes, path)]['data']) for path in baselines}
        changed = {path: value for path, value in saved.items() if value != baselines[path]}
        final = report['counter']['final']['value']
        evidence.check('stopped-disk per-slot Counter save equals decoded final paused value', len(changed) == 1 and
                       list(changed.values()) == [final] and final > report['counter']['initial']['value'],
                       changed=changed, final_visible=final, seeded=baselines)
        report['volume'] = dict(file=disk.name, bytes=len(raw), sha256=sha256(raw), slot=slot, generation=generation,
                                counter_files=saved)
        serial = session.log.read_text()
        evidence.check('serial remains free of PANIC', 'PANIC:' not in serial)
        report['passed'] = True
    except BaseException as error:
        report['error'] = f'{type(error).__name__}: {error}'
        # Evidence may outlive a failed run; it never becomes a passing report.
        raise
    finally:
        if session is not None:
            if session.log.exists():
                (output / 'serial.log').write_text(session.log.read_text())
            stderr = session.directory / 'stderr.log'
            if stderr.exists():
                (output / 'qemu-stderr.log').write_bytes(stderr.read_bytes())
        evidence.save()
    print(json.dumps(report, indent=2), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--profile', choices=('default', 'large'), default='default')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix='baseos-native-pointer-')) / 'evidence'
    run(args.build.resolve(), args.profile, output.resolve())
