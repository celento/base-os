"""The fixed part of the runtime palette, replicated for asset tools.

gfx.c owns the layout: 16 base colors, a 32-step ink ramp (set by the kernel
theme to the values below), 16 wallpaper colors, 48 icon colors, 16 accent
steps and a 5x5x5 cube. Asset tools quantize against the entries that never
change at runtime: base, ink and cube, plus the icon palette they emit.
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

PAL_GRAY, PAL_GRAY_N = 16, 32
PAL_DESK, PAL_DESK_N = 48, 16
PAL_ICON, PAL_ICON_N = 64, 48
PAL_ACCENT, PAL_ACCENT_N = 112, 16
PAL_CUBE = 128
CUBE_LEVELS = (0, 64, 128, 192, 255)

# Ink ramp of the light theme: INK(0) is the brightest surface, INK(31) the
# darkest text. Two segments, exactly as src/kernel.c theme_apply sets them.
INK_SEGMENTS = ((PAL_GRAY, 17, 0xF7F8FA, 0x8A9099), (PAL_GRAY + 16, 16, 0x8A9099, 0x14161B))


def lerp_rgb(a, b, t, n):
    """Integer lerp identical to gfx.c (C division truncates toward zero)."""
    def chan(shift):
        x0, x1 = (a >> shift) & 255, (b >> shift) & 255
        d = (x1 - x0) * t
        return x0 + (abs(d) // n) * (1 if d >= 0 else -1)
    return (chan(16) << 16) | (chan(8) << 8) | chan(0)


def base_colors():
    source = (ROOT / "src/gfx.c").read_text()
    body = re.search(r"pal_base\[16\]\s*=\s*\{(.*?)\};", source, re.S)[1]
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", body)]


def fixed_palette():
    """index -> 0xRRGGBB for every entry that is constant at runtime."""
    colors = {}
    for i, c in enumerate(base_colors()):
        colors[i] = c
    for base, n, c0, c1 in INK_SEGMENTS:
        for i in range(n):
            colors[base + i] = lerp_rgb(c0, c1, i, n - 1)
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
