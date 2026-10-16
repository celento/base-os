"""Draw the default Blue wallpaper: a deep blue gradient crossed by soft,
blurred light ribbons. Writes assets/wallpapers/blue.jpg; run
tools/gen_wallpapers.py afterwards to rebundle.
"""
import math
import pathlib

from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).resolve().parents[1]
W, H, SS = 640, 360, 2


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    w, h = W * SS, H * SS
    top, mid, bottom = (8, 48, 130), (26, 104, 205), (112, 190, 240)
    base = Image.new("RGB", (w, h))
    px = base.load()
    for y in range(h):
        t = y / (h - 1)
        c = lerp(top, mid, t / 0.55) if t < 0.55 else lerp(mid, bottom, (t - 0.55) / 0.45)
        for x in range(w):
            # A gentle light source in the upper left.
            d = math.hypot(x / w - 0.22, y / h - 0.1)
            glow = max(0.0, 1.0 - d * 1.6) ** 2 * 60
            px[x, y] = tuple(min(255, int(v + glow)) for v in c)
    ribbons = Image.new("RGB", (w, h), (0, 0, 0))
    draw = ImageDraw.Draw(ribbons)
    for k, (amp, freq, phase, width, bright) in enumerate(((0.16, 1.3, 0.0, 90, 70), (0.11, 1.9, 1.6, 60, 55), (0.08, 2.6, 3.4, 40, 45))):
        points = []
        for x in range(0, w + 8, 8):
            y = h * (0.52 + amp * math.sin(freq * 2 * math.pi * x / w + phase) + 0.08 * k)
            points.append((x, y))
        draw.line(points, fill=(bright, bright, bright), width=width * SS, joint="curve")
    ribbons = ribbons.filter(ImageFilter.GaussianBlur(22 * SS))
    out = Image.new("RGB", (w, h))
    op, bp, rp = out.load(), base.load(), ribbons.load()
    for y in range(h):
        for x in range(w):
            r, g, b = bp[x, y]
            l = rp[x, y][0]
            op[x, y] = (min(255, r + l // 2), min(255, g + l * 2 // 3), min(255, b + l))
    out = out.resize((W, H), Image.LANCZOS)
    out.save(ROOT / "assets/wallpapers/blue.jpg", quality=72, optimize=True, progressive=False, subsampling=2)
    print("wrote assets/wallpapers/blue.jpg")


if __name__ == "__main__":
    main()
