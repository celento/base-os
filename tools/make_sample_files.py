"""Generate the original sample files that new data disks carry in */Sample.

python3 tools/make_sample_files.py            # rewrites assets/samples
python3 tools/make_sample_files.py build/out  # elsewhere

Everything is drawn, written or synthesized here (Python + Pillow); nothing is
downloaded. The files are MIT-licensed with BaseOS. The two Harbor media files
in Media/Sample come from assets/examples and are added by tools/sample_pack.py.
"""
import argparse
import io
import math
import pathlib
import struct
import sys
import wave

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_image_examples import landscape  # noqa: E402
from palette import fixed_palette, nearest  # noqa: E402


# ---------- Pictures ----------

def dunes(width=480, height=300):
    im = Image.new('RGB', (width, height))
    p = im.load()
    for y in range(height):
        t = y / (height - 1)
        top, bottom = (44, 38, 96), (247, 160, 92)
        row = tuple(int(a + (b - a) * min(1.0, t * 1.6)) for a, b in zip(top, bottom))
        for x in range(width):
            p[x, y] = row
    d = ImageDraw.Draw(im)
    d.ellipse((300, 120, 380, 200), fill=(255, 214, 120))
    for i, (base, colour) in enumerate([(210, (196, 108, 62)), (238, (170, 86, 50)), (268, (132, 62, 40))]):
        points = [(0, height)]
        for x in range(0, width + 1, 8):
            points.append((x, base + 18 * math.sin(x / (61 + 17 * i) + i * 1.7)))
        points.append((width, height))
        d.polygon(points, fill=colour)
    for x in (70, 92, 104):
        d.line((x, 232, x, 214 - (x % 7)), fill=(60, 34, 30), width=2)
    return im.quantize(colors=48)


def tiles(width=320, height=240, cell=40):
    colours = [(38, 70, 83), (42, 157, 143), (233, 196, 106), (244, 162, 97), (231, 111, 81)]
    im = Image.new('RGB', (width, height), (250, 246, 238))
    d = ImageDraw.Draw(im)
    for row in range(height // cell):
        for col in range(width // cell):
            x, y = col * cell, row * cell
            a = colours[(row * 2 + col) % len(colours)]
            b = colours[(row + col * 3 + 2) % len(colours)]
            d.rectangle((x, y, x + cell - 1, y + cell - 1), fill=a)
            kind = (row * 7 + col * 3) % 4
            if kind == 0:
                d.pieslice((x - cell, y - cell, x + cell - 1, y + cell - 1), 0, 90, fill=b)
            elif kind == 1:
                d.ellipse((x + 8, y + 8, x + cell - 9, y + cell - 9), fill=b)
            elif kind == 2:
                d.polygon([(x, y + cell - 1), (x + cell - 1, y), (x + cell - 1, y + cell - 1)], fill=b)
            else:
                d.rectangle((x + 12, y + 12, x + cell - 13, y + cell - 13), fill=b)
    return im.quantize(colors=8)


def robot():
    """A 160 x 100 drawing in Paint's own BOS1 format, on the fixed palette."""
    im = Image.new('RGB', (160, 100), (255, 255, 255))
    d = ImageDraw.Draw(im)
    d.rectangle((0, 78, 159, 99), fill=(128, 192, 128))
    d.ellipse((124, 8, 148, 32), fill=(255, 255, 0))
    d.rectangle((60, 30, 100, 62), fill=(128, 128, 192), outline=(0, 0, 64))
    d.rectangle((66, 8, 94, 28), fill=(192, 192, 255), outline=(0, 0, 64))
    d.line((80, 8, 80, 2), fill=(0, 0, 64))
    d.ellipse((77, 0, 83, 4), fill=(255, 0, 0))
    d.rectangle((71, 14, 76, 19), fill=(0, 0, 0))
    d.rectangle((84, 14, 89, 19), fill=(0, 0, 0))
    d.line((72, 23, 88, 23), fill=(0, 0, 64))
    d.rectangle((48, 32, 58, 54), fill=(128, 128, 192), outline=(0, 0, 64))
    d.rectangle((102, 32, 112, 54), fill=(128, 128, 192), outline=(0, 0, 64))
    d.rectangle((64, 64, 76, 84), fill=(64, 64, 128))
    d.rectangle((84, 64, 96, 84), fill=(64, 64, 128))
    d.rectangle((70, 38, 90, 48), fill=(255, 128, 0))
    colours = fixed_palette()
    cache = {}
    pixels = bytearray()
    for y in range(100):
        for x in range(160):
            rgb = im.getpixel((x, y))
            key = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
            if key not in cache:
                cache[key] = nearest(colours, key)
            pixels.append(cache[key])
    return b'BOS1' + struct.pack('<HH', 160, 100) + bytes(pixels)


# ---------- Documents ----------

def writer(paragraphs):
    """BaseOS Writer v1: (text, paragraph flags, style for the whole line)."""
    text = ''.join(line + '\n' for line, _, _ in paragraphs).encode('ascii')
    assert len(text) <= 32768
    styles, flags = bytearray(len(text) + 1), bytearray(len(text) + 1)
    offset = 0
    for line, paragraph, style in paragraphs:
        count = len(line.encode('ascii'))
        flags[offset] = paragraph
        styles[offset:offset + count] = bytes([style]) * count
        offset += count + 1
    return struct.pack('<4sHHII', b'BWR1', 1, 0, len(text), 0) + text + styles + flags


LETTER = [
    ('Welcome to BaseOS', 5, 1),
    ('A short letter to try Writer with', 1, 2),
    ('', 0, 0),
    ('Hello,', 0, 0),
    ('This folder holds a few sample files so every app has something to open right away. They are yours to change, move or delete; BaseOS never puts them back once a disk is set up.', 0, 0),
    ('What to try', 4, 1),
    ('Select a word and press Bold, Italic or Underline. Put the cursor in a line and choose Heading, or align it Left, Center or Right.', 0, 0),
    ('Press Ctrl+F to find text and Ctrl+H to replace it. Ctrl+Z undoes a step.', 0, 0),
    ('Ctrl+S saves this document. Ctrl+Shift+E exports rich text and Ctrl+Shift+P exports a PDF.', 0, 0),
    ('Other samples', 4, 1),
    ('Trip notes.txt opens in Editor, Trip budget.bsh and Reading log.csv in Spreadsheet, and Pancakes.html in Browser.', 0, 0),
    ('Pictures/Sample has images for Image Viewer and a drawing for Paint. Media/Sample has a sound, a song and a short video for Media Player.', 0, 0),
    ('', 0, 0),
    ('Have fun!', 2, 2),
]

NOTES = """Trip notes: a weekend by the lake

Friday
- Leave after lunch, about three hours of driving
- Pick up the cabin key at the visitor center before 6 pm
- Dinner: pasta and salad

Saturday
- Morning walk around the lake (8 km, mostly flat)
- Rent a canoe at the north beach
- Picnic on the island if the wind is calm
- Evening: stars, cards, early night

Sunday
- Pancakes (recipe in Pancakes.html)
- Pack up and leave the cabin by 11 am
- Stop at the farm stand on the way home

Packing list
[ ] Rain jacket          [ ] Hiking shoes
[ ] Sweater              [ ] Swimsuit and towel
[ ] Flashlight           [ ] Water bottles
[ ] Sunscreen            [ ] Book for the evening

This is a plain text file. Edit it in Editor and press Ctrl+S to save.
"""

READING_LOG = (
    'Title,Pages,Days,Pages per day,Rating\r\n'
    'The Quiet Harbor,312,9,34.7,4\r\n'
    'Notes on Gardening,188,5,37.6,5\r\n'
    'A Short History of Maps,264,12,22,3\r\n'
    'Night Train North,401,10,40.1,4\r\n'
    'Cooking for Friends,156,4,39,5\r\n'
    ',,,,\r\n'
    'All titles are made up for this example.,,,,\r\n'
)

PANCAKES = """<title>Weekend pancakes</title>
<h1>Weekend pancakes</h1>
<p>A simple recipe for about twelve small pancakes. It is a sample page for the BaseOS Browser.</p>
<h2>Ingredients</h2>
<ul>
<li>200 g flour</li>
<li>2 teaspoons baking powder</li>
<li>1 tablespoon sugar and a pinch of salt</li>
<li>2 eggs</li>
<li>300 ml milk</li>
<li>2 tablespoons melted butter, plus a little for the pan</li>
</ul>
<h2>Steps</h2>
<ol>
<li>Mix the flour, baking powder, sugar and salt in a bowl.</li>
<li>Whisk the eggs, milk and melted butter in a jug.</li>
<li>Pour the wet mix into the dry mix and stir until just combined. A few lumps are fine.</li>
<li>Heat a buttered pan on medium. Pour in small rounds of batter.</li>
<li>Flip when bubbles appear on top, then cook one more minute.</li>
</ol>
<h2>Ideas</h2>
<p>Serve with berries, yogurt or maple syrup. Add a mashed banana to the batter for a sweeter version.</p>
<p>The trip that goes with these pancakes is in <b>Trip notes.txt</b>.</p>
"""


def sheet(cells, widths=None):
    """BaseOS Spreadsheet v2: cells are (cell name, source, format byte)."""
    widths = widths or {}
    records = []
    for name, source, fmt in cells:
        column, row = ord(name[0]) - 65, int(name[1:]) - 1
        if source.startswith('='):
            kind = 3
        else:
            try:
                float(source)
                kind = 2
            except ValueError:
                kind = 1
        data = source.encode('ascii')
        assert len(data) <= 95
        records.append((row * 26 + column, kind, fmt, data))
    records.sort()
    out = bytearray(struct.pack('<4sHHHHI', b'BSH1', 2, 0, 128, 26, len(records)))
    out += b''.join(struct.pack('<H', widths.get(chr(65 + c), 104)) for c in range(26))
    for index, kind, fmt, data in records:
        out += struct.pack('<HBBB', index, kind, len(data), fmt) + data
    return bytes(out)


def trip_budget():
    money, items = 2, [('Fuel', '60', '54.20'), ('Cabin, two nights', '240', '240'),
                       ('Groceries', '90', '76.85'), ('Canoe rental', '35', '35'),
                       ('Farm stand', '25', '31.50')]
    cells = [('A1', 'Trip budget', 0), ('A2', 'Item', 0), ('B2', 'Planned', 0),
             ('C2', 'Spent', 0), ('D2', 'Left', 0)]
    for i, (item, planned, spent) in enumerate(items, start=3):
        cells += [(f'A{i}', item, 0), (f'B{i}', planned, money), (f'C{i}', spent, money),
                  (f'D{i}', f'=B{i}-C{i}', money)]
    last = 2 + len(items)
    total = last + 2
    cells += [(f'A{total}', 'Total', 0), (f'B{total}', f'=SUM(B3:B{last})', money),
              (f'C{total}', f'=SUM(C3:C{last})', money), (f'D{total}', f'=B{total}-C{total}', money),
              (f'A{total + 1}', 'Share of plan spent', 0), (f'C{total + 1}', f'=C{total}/B{total}', 3),
              (f'A{total + 3}', 'Change the Spent column and the totals follow.', 0)]
    return sheet(cells, {'A': 160})


# ---------- Media ----------

def doorbell():
    """A two-note chime: 22,050 Hz, 16-bit mono PCM."""
    rate, frames = 22050, bytearray()
    total = int(rate * 1.6)
    for i in range(total):
        t = i / rate
        value = 0.0
        for start, frequency in ((0.0, 659.255), (0.45, 523.251)):
            if t >= start:
                age = t - start
                envelope = min(1.0, age * 200) * math.exp(-age * 3.2)
                value += envelope * (math.sin(2 * math.pi * frequency * age) +
                                     0.3 * math.sin(4 * math.pi * frequency * age))
        frames += struct.pack('<h', max(-32767, min(32767, int(value * 9000))))
    buffer = io.BytesIO()
    with wave.open(buffer, 'wb') as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(rate)
        output.writeframes(bytes(frames))
    return buffer.getvalue()


def encode(image, fmt, **options):
    buffer = io.BytesIO()
    image.save(buffer, fmt, **options)
    return buffer.getvalue()


def files():
    return {
        'Pictures/Sample/Mountain lake.jpg': encode(landscape(), 'JPEG', quality=88, subsampling=0),
        'Pictures/Sample/Sunset dunes.png': encode(dunes(), 'PNG', optimize=True),
        'Pictures/Sample/Tiles.gif': encode(tiles(), 'GIF'),
        'Pictures/Sample/Robot.pbm': robot(),
        'Documents/Sample/Welcome letter.bwr': writer(LETTER),
        'Documents/Sample/Trip notes.txt': NOTES.encode('ascii'),
        'Documents/Sample/Trip budget.bsh': trip_budget(),
        'Documents/Sample/Reading log.csv': READING_LOG.encode('ascii'),
        'Documents/Sample/Pancakes.html': PANCAKES.encode('ascii'),
        'Media/Sample/Doorbell.wav': doorbell(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=pathlib.Path, nargs='?', default=ROOT / 'assets/samples')
    out = parser.parse_args().output
    for name, data in files().items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f'{path}: {len(data):,} bytes')


if __name__ == '__main__':
    main()
