"""Rasterize the UI icon set (assets/ui-icons/<style>/*.svg) into src/ui/ui_icons.h.

    python3 tools/gen_ui_icons.py [--sheet preview.png]

Needs Playwright's Chromium (PLAYWRIGHT_MODULE may point at the module) and
Pillow; normal builds only need the committed header.

Every icon exists once per style (Classic and Modern) with the same name. A
16-unit viewBox gives a 16 px icon; a 32-unit viewBox gives 32 and 16 px; the
logo is drawn at several sizes. Icons are rendered at 4x and each output pixel
is classified from its 16 subsamples: the share of opaque samples becomes a
4-bit alpha, the most common source color becomes the pixel's palette index,
and a second color covering at least two samples becomes a mix that the
runtime blends with gfx_mix. Blending at draw time against live palette
entries keeps accent-colored art correct when the accent changes.

Colors resolve against the style's palette as src/ui/theme.c defines it:
tokens and icon colors when they match exactly (the desktop entry never
matches), then the fixed gray ramp and cube, and anything else gets an entry
in the 16-color extra band. currentColor becomes the caller's ink (index 255)
and #00FFFF the caller's paper (index 254).
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "assets/ui-icons"
OUT = ROOT / "src/ui/ui_icons.h"
STYLES = ("classic", "modern")
SS = 4
INK, PAPER = 255, 254
INK_RGB, PAPER_RGB = 0xFF00FF, 0x00FFFF
PAL_TOKEN, PAL_ICON, PAL_GRAY, PAL_ICON_X, PAL_CUBE = 16, 48, 80, 112, 128
TK_DESKTOP = 26
LOGO_SIZES = (16, 20, 24, 32, 48, 128)


def theme_tables(style):
    source = (ROOT / "src/ui/theme.c").read_text()

    def table(name):
        body = re.search(name + r"\[\d+\]\s*=\s*\{(.*?)\};", source, re.S)[1]
        return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", body)]
    return table(style + "_tokens"), table(style + "_icon")


def distance(a, b):
    dr, dg, db = ((a >> 16) & 255) - ((b >> 16) & 255), ((a >> 8) & 255) - ((b >> 8) & 255), (a & 255) - (b & 255)
    return dr * dr * 3 + dg * dg * 4 + db * db * 2


def fixed_entries():
    colors = {}
    for i in range(32):
        v = i * 255 // 31
        colors[PAL_GRAY + i] = (v << 16) | (v << 8) | v
    levels = (0, 64, 128, 192, 255)
    for i in range(125):
        colors[PAL_CUBE + i] = (levels[i // 25] << 16) | (levels[(i // 5) % 5] << 8) | levels[i % 5]
    return colors


def svg_colors(text):
    return sorted({int(c, 16) for c in re.findall(r"#([0-9A-Fa-f]{6})\b", text)} | {INK_RGB})


def sizes_for(name, text):
    if name == "logo":
        return LOGO_SIZES
    box = int(float(re.search(r'viewBox="[\d.]+ [\d.]+ ([\d.]+)', text)[1]))
    return (32, 16) if box >= 32 else (16,)


def render(files, size, work):
    env = dict(os.environ)
    env.setdefault("PLAYWRIGHT_MODULE", "playwright")
    subprocess.run(["node", str(ROOT / "tools/svg_render.js"), str(work), str(size * SS), *map(str, files)],
                   check=True, env=env)


def classify(im, size, sources):
    """Per output pixel: (alpha 0..15, primary rgb, secondary rgb or None, level 0..255)."""
    px = im.load()
    cache = {}

    def nearest(rgb):
        if rgb not in cache:
            cache[rgb] = min(sources, key=lambda c: distance(c, rgb))
        return cache[rgb]
    out = []
    for y in range(size):
        for x in range(size):
            counts = {}
            for sy in range(SS):
                for sx in range(SS):
                    r, g, b, a = px[x * SS + sx, y * SS + sy]
                    if a < 128:
                        continue
                    c = nearest((r << 16) | (g << 8) | b)
                    counts[c] = counts.get(c, 0) + 1
            n = sum(counts.values())
            if not n:
                out.append((0, 0, None, 0))
                continue
            ranked = sorted(counts.items(), key=lambda kv: -kv[1])
            alpha = (n * 15 + 8) // 16
            if alpha == 0:
                alpha = 1
            primary, c1 = ranked[0]
            second, level = None, 0
            if len(ranked) > 1 and ranked[1][1] >= 2:
                second, c2 = ranked[1]
                level = c2 * 256 // (c1 + c2)
            out.append((alpha, primary, second, level))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sheet", type=pathlib.Path)
    args = parser.parse_args()
    names = sorted(p.stem for p in (SRC / "classic").glob("*.svg"))
    for style in STYLES:
        have = sorted(p.stem for p in (SRC / style).glob("*.svg"))
        if have != names:
            sys.exit(f"{style} icons differ from classic: {sorted(set(have) ^ set(names))}")

    fixed = fixed_entries()
    entries = {}   # (style, name, size) -> pixels
    extras = {}
    with tempfile.TemporaryDirectory() as temp:
        work = pathlib.Path(temp)
        for style in STYLES:
            groups = {}
            texts = {}
            for name in names:
                text = (SRC / style / f"{name}.svg").read_text()
                texts[name] = text
                d = work / style
                d.mkdir(exist_ok=True)
                f = d / f"{name}.svg"
                f.write_text(text.replace("currentColor", "#FF00FF"))
                for size in sizes_for(name, text):
                    groups.setdefault(size, []).append(f)
            for size, files in groups.items():
                render(files, size, work / style)
                for f in files:
                    im = Image.open(work / style / f"{f.stem}-{size * SS}.png").convert("RGBA")
                    entries[(style, f.stem, size)] = classify(im, size, svg_colors(texts[f.stem]))

        for style in STYLES:
            tokens, icon = theme_tables(style)
            exact = {}
            for i, c in enumerate(tokens):
                if PAL_TOKEN + i != TK_DESKTOP:
                    exact.setdefault(c, PAL_TOKEN + i)
            for i, c in enumerate(icon):
                exact.setdefault(c, PAL_ICON + i)
            used = set()
            for (st, _n, _s), pixels in entries.items():
                if st == style:
                    for _a, p, s, _l in pixels:
                        used.add(p)
                        if s is not None:
                            used.add(s)
            mapping = {INK_RGB: INK, PAPER_RGB: PAPER}
            band = []
            for c in sorted(used - {INK_RGB, PAPER_RGB, 0}):
                if c in exact:
                    mapping[c] = exact[c]
                    continue
                near = min(fixed, key=lambda i: distance(fixed[i], c))
                if distance(fixed[near], c) <= 12:
                    mapping[c] = near
                    continue
                band.append(c)
            if len(band) > 16:
                sys.exit(f"{style}: {len(band)} extra icon colors, the band holds 16")
            for i, c in enumerate(band):
                mapping[c] = PAL_ICON_X + i
            extras[style] = (band + [0x808080] * 16)[:16], mapping

    order = [(name, size) for name in names for size in sizes_for(name, (SRC / "classic" / f"{name}.svg").read_text())]
    lines = ["/* Generated by tools/gen_ui_icons.py from assets/ui-icons. Do not edit.",
             " * Per image: size*size palette indices (255 ink, 254 paper), size*size/2",
             " * bytes of 4-bit alpha, then mixes as (pixel offset lo, hi, index, level). */",
             "#ifndef UI_ICONS_H", "#define UI_ICONS_H", "#include <stdint.h>", "", "enum {"]
    lines += [f"    UI_ICON_{n.upper().replace('-', '_')}," for n in names]
    lines += ["    UI_ICON_COUNT", "};", "",
              "typedef struct { uint8_t icon, size; } UiIconImage;",
              f"#define UI_ICON_IMAGES {len(order)}",
              "extern const uint32_t ui_icon_extra[2][16];",
              "extern const UiIconImage ui_icon_images[UI_ICON_IMAGES];",
              "/* Per style: {offset into ui_icon_data[style], mix count} per image. */",
              "extern const uint32_t (*const ui_icon_offsets[2])[2];",
              "extern const uint8_t *const ui_icon_data[2];", "", "#ifdef UI_ICONS_DEFINE_DATA",
              "const uint32_t ui_icon_extra[2][16] = {"]
    for style in STYLES:
        lines.append("    {" + ", ".join(f"0x{c:06X}" for c in extras[style][0]) + "},")
    lines.append("};")
    blobs = {}
    images = {}
    for style in STYLES:
        mapping = extras[style][1]
        blob = []
        for name, size in order:
            pixels = entries[(style, name, size)]
            idx = [mapping.get(p, 0) if a else 0 for a, p, _s, _l in pixels]
            al = [a for a, _p, _s, _l in pixels]
            mixes = [(i, mapping[s], lv) for i, (a, _p, s, lv) in enumerate(pixels) if a and s is not None]
            images.setdefault(style, []).append((name, size, len(mixes), len(blob)))
            blob += idx
            blob += [(al[i] << 4) | al[i + 1] for i in range(0, size * size, 2)]
            for i, ix, lv in mixes:
                blob += [i & 255, i >> 8, ix, min(lv, 255)]
        blobs[style] = blob
    lines.append("const UiIconImage ui_icon_images[UI_ICON_IMAGES] = {")
    for (name, size, _m, _o) in images["classic"]:
        lines.append(f"    {{UI_ICON_{name.upper().replace('-', '_')}, {size}}},")
    lines.append("};")
    for style in STYLES:
        lines.append(f"static const uint32_t ui_icon_offsets_{style}[UI_ICON_IMAGES][2] = {{")
        lines.append("    " + ", ".join(f"{{{o}, {m}}}" for (_n, _s, m, o) in images[style]))
        lines.append("};")
        data = blobs[style]
        lines.append(f"static const uint8_t ui_icon_blob_{style}[{len(data)}] = {{")
        for i in range(0, len(data), 40):
            lines.append("    " + ",".join(str(v) for v in data[i:i + 40]) + ",")
        lines.append("};")
    lines += ["const uint8_t *const ui_icon_data[2] = {ui_icon_blob_classic, ui_icon_blob_modern};",
              "const uint32_t (*const ui_icon_offsets[2])[2] = {ui_icon_offsets_classic, ui_icon_offsets_modern};",
              "#endif", "", "#endif", ""]
    OUT.write_text("\n".join(lines))

    if args.sheet:
        sheet_out(args.sheet, names, order, entries, extras)
    total = sum(len(b) for b in blobs.values())
    print(f"Wrote {len(names)} icons, {len(order)} images per style, {total} bytes to {OUT.relative_to(ROOT)}")


def sheet_out(path, names, order, entries, extras):
    order = [(n, s) for n, s in order if s <= 32]
    cols, cell = 12, 40
    rows = (len(order) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell * 2, rows * cell), (233, 237, 242))
    for si, style in enumerate(STYLES):
        for k, (name, size) in enumerate(order):
            x0 = si * cols * cell + (k % cols) * cell + 4
            y0 = (k // cols) * cell + 4
            for i, (a, p, s, lv) in enumerate(entries[(style, name, size)]):
                if not a:
                    continue
                c = 0x3C434C if p in (INK_RGB,) else 0xFFFFFF if p == PAPER_RGB else p
                if s is not None:
                    s2 = 0x3C434C if s == INK_RGB else 0xFFFFFF if s == PAPER_RGB else s
                    c = mix(c, s2, lv)
                bg = (233 << 16) | (237 << 8) | 242
                c = mix(bg, c, a * 17)
                sheet.putpixel((x0 + i % size, y0 + i // size), ((c >> 16) & 255, (c >> 8) & 255, c & 255))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(path)


def mix(a, b, t):
    def ch(s):
        x, y = (a >> s) & 255, (b >> s) & 255
        return x + (y - x) * t // 256
    return (ch(16) << 16) | (ch(8) << 8) | ch(0)


if __name__ == "__main__":
    main()
