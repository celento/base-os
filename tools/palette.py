"""The fixed part of the runtime palette, replicated for asset tools.

src/kernel/gfx.h owns the layout: 16 base colors, the UI theme bands (32
tokens, 16 icon colors, 16 terminal colors, then 16 extra icon colors at
112), a 32-step neutral gray ramp at 80 and a 5x5x5 cube at 128. The theme
bands change with the style and accent, so asset tools quantize only against
the entries that never change: base, gray ramp and cube.
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

PAL_TOKEN, PAL_TOKEN_N = 16, 32
PAL_ICON, PAL_ICON_N = 48, 16
PAL_ANSI, PAL_ANSI_N = 64, 16
PAL_GRAY, PAL_GRAY_N = 80, 32
PAL_ICON_X, PAL_ICON_X_N = 112, 16
PAL_CUBE = 128
CUBE_LEVELS = (0, 64, 128, 192, 255)


def lerp_rgb(a, b, t, n):
    """Integer lerp identical to gfx.c (C division truncates toward zero)."""
    def chan(shift):
        x0, x1 = (a >> shift) & 255, (b >> shift) & 255
        d = (x1 - x0) * t
        return x0 + (abs(d) // n) * (1 if d >= 0 else -1)
    return (chan(16) << 16) | (chan(8) << 8) | chan(0)


def base_colors():
    source = (ROOT / "src/kernel/gfx.c").read_text()
    body = re.search(r"pal_base\[16\]\s*=\s*\{(.*?)\};", source, re.S)[1]
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", body)]


def fixed_palette():
    """index -> 0xRRGGBB for every entry that is constant at runtime."""
    colors = {}
    for i, c in enumerate(base_colors()):
        colors[i] = c
    for i in range(PAL_GRAY_N):
        v = i * 255 // (PAL_GRAY_N - 1)
        colors[PAL_GRAY + i] = (v << 16) | (v << 8) | v
    for i in range(125):
        colors[PAL_CUBE + i] = (CUBE_LEVELS[i // 25] << 16) | (CUBE_LEVELS[(i // 5) % 5] << 8) | CUBE_LEVELS[i % 5]
    return colors


def distance(a, b):
    """gfx_rgb's weighted distance."""
    dr, dg, db = ((a >> 16) & 255) - ((b >> 16) & 255), ((a >> 8) & 255) - ((b >> 8) & 255), (a & 255) - (b & 255)
    return dr * dr * 3 + dg * dg * 4 + db * db * 2


def nearest(colors, rgb):
    best, bd = 0, None
    for index, c in colors.items():
        d = distance(c, rgb)
        if bd is None or d < bd:
            best, bd = index, d
    return best


def median_cut(pixels, count):
    """Classic median cut over (r, g, b) tuples; returns up to `count` colors."""
    boxes = [list(pixels)]
    while len(boxes) < count:
        boxes.sort(key=lambda b: -len(b))
        box = boxes.pop(0)
        if len(box) < 2:
            boxes.append(box)
            break
        ranges = [max(p[k] for p in box) - min(p[k] for p in box) for k in range(3)]
        axis = ranges.index(max(ranges))
        box.sort(key=lambda p: p[axis])
        mid = len(box) // 2
        boxes += [box[:mid], box[mid:]]
    result = []
    for box in boxes:
        if not box:
            continue
        n = len(box)
        result.append(tuple(sum(p[k] for p in box) // n for k in range(3)))
    return result
