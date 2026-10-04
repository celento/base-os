"""Generate original, ordinary image examples and independent pixel references.
Requires Pillow only when regenerating. Generated artwork is MIT-licensed with
BaseOS; no downloaded image, private data, fuzz input, or exploit sample is used.
"""
import argparse
import hashlib
import json
import math
import pathlib
import struct
import zlib
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]


def landscape(width=640, height=360):
    im = Image.new('RGB', (width, height))
    p = im.load()
    for y in range(height):
        t = y / (height - 1)
        for x in range(width):
            p[x, y] = (int(53 + 104 * t), int(116 + 69 * t), int(169 + 42 * t))
    d = ImageDraw.Draw(im)
    d.ellipse((466, 28, 536, 98), fill=(255, 221, 142))
    d.ellipse((70, 38, 148, 66), fill=(215, 233, 238))
    d.ellipse((112, 28, 181, 67), fill=(230, 241, 240))
    d.ellipse((155, 42, 232, 70), fill=(218, 235, 238))
    d.polygon([(0, 206), (89, 107), (181, 204), (294, 76), (435, 214), (537, 128), (640, 205), (640, 300), (0, 300)], fill=(81, 122, 151))
    d.polygon([(243, 135), (294, 76), (355, 136), (313, 120), (293, 138), (279, 116)], fill=(228, 235, 228))
    d.polygon([(61, 139), (89, 107), (119, 139), (91, 130)], fill=(221, 232, 230))
    d.polygon([(0, 258), (115, 180), (213, 259), (398, 161), (507, 256), (607, 197), (640, 214), (640, 360), (0, 360)], fill=(48, 100, 108))
    d.polygon([(0, 260), (170, 248), (300, 261), (440, 248), (640, 264), (640, 360), (0, 360)], fill=(41, 118, 151))
    for y in range(270, 360, 7):
        x = 110 + (y * 29 % 170)
        d.line((x, y, 470 + (y % 47), y), fill=(87, 160, 173), width=1)
    d.polygon([(0, 257), (70, 278), (194, 324), (160, 360), (0, 360)], fill=(24, 67, 68))
    d.polygon([(640, 270), (573, 276), (463, 339), (504, 360), (640, 360)], fill=(23, 73, 66))
    for x, y, height in [(26, 310, 92), (75, 323, 104), (126, 346, 69), (594, 324, 105), (548, 344, 70), (621, 350, 104)]:
        d.rectangle((x - 2, y - 25, x + 2, y), fill=(75, 65, 44))
        for offset in (0, 18, 36):
            top = y - height + offset
            spread = 14 + offset // 3
            d.polygon([(x, top), (x - spread, top + 43), (x + spread, top + 43)], fill=(20, 60 + offset // 3, 53))
    return im


def rgba_example():
    im = Image.new('RGBA', (160, 112), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((9, 9, 151, 103), radius=22, fill=(36, 111, 200, 180))
    d.ellipse((26, 22, 87, 83), fill=(247, 184, 60, 255))
    d.ellipse((63, 32, 125, 94), fill=(223, 57, 111, 155))
    for x in range(160):
        d.line((x, 104, x, 111), fill=(58, 201, 147, x * 255 // 159))
    return im


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def adam7(image):
    im = image.convert('RGBA'); width, height = im.size; pixels = im.load()
    raw = bytearray()
    for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
        for y in range(y0, height, dy):
            row = bytes(component for x in range(x0, width, dx) for component in pixels[x, y])
            if row: raw.extend(b'\0' + row)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 1)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'tests/fixtures/images')
    args = parser.parse_args(); out = args.output; out.mkdir(parents=True, exist_ok=True)
    picture = landscape()
    picture.save(out / 'landscape.png')
    picture.save(out / 'landscape.jpg', quality=91, subsampling=0)
    picture.resize((192, 108)).save(out / 'progressive.jpg', quality=91, progressive=True, subsampling=0)
    picture.resize((128, 72)).convert('L').save(out / 'grayscale.jpg', quality=91)
    small = picture.resize((96, 54))
    small.save(out / 'rgb.bmp')
    small.quantize(colors=32).save(out / 'palette.bmp')
    small.quantize(colors=32).save(out / 'palette.png')
    alpha = rgba_example(); alpha.save(out / 'transparent.png')
    (out / 'interlaced.png').write_bytes(adam7(alpha.resize((71, 43))))
    alpha.convert('LA').save(out / 'gray_alpha.png')
    mono = Image.new('1', (31, 23)); md = ImageDraw.Draw(mono); md.rectangle((3, 4, 25, 18), fill=1); mono.save(out / 'monochrome.png')
    palette = small.quantize(colors=16)
    palette.save(out / 'animated.gif', save_all=True, append_images=[palette.transpose(Image.Transpose.FLIP_LEFT_RIGHT)], loop=0, duration=[100, 100])
    transparent = alpha.quantize(colors=63); transparent.save(out / 'transparent.gif', transparency=0)
    gray = Image.new('I;16', (53, 29)); gray.putdata([int(65535 * (x + y) / 80) for y in range(29) for x in range(53)]); gray.save(out / 'gray16.png')
    bos = bytes([128 + (x // 8 % 5) * 25 + (y // 8 % 5) * 5 + (x // 24 % 5) for y in range(32) for x in range(48)])
    (out / 'paint.pbm').write_bytes(b'BOS1' + struct.pack('<HH', 48, 32) + bos)
    manifest = []
    format_ids = {'.jpg': 2, '.png': 3, '.bmp': 4, '.gif': 5, '.pbm': 1}
    for path in sorted(out.iterdir()):
        if path.suffix not in format_ids: continue
        data = path.read_bytes(); kind = format_ids[path.suffix]
        if kind == 1:
            width, height = 48, 32; expected = bos
        else:
            with Image.open(path) as im:
                width, height = im.size
                if path.name == 'gray16.png':
                    expected = bytes(v for value in im.getdata() for v in [value >> 8] * 3 + [255])
                else:
                    expected = im.convert('RGBA').tobytes()
        reference = path.name + '.rgba.zlib'
        (out / reference).write_bytes(zlib.compress(expected, 9))
        manifest.append(dict(name=path.name, width=width, height=height, format=kind,
                             sha256=hashlib.sha256(data).hexdigest(), reference=reference))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Wrote {len(manifest)} ordinary reference images to {out}')


if __name__ == '__main__': main()
