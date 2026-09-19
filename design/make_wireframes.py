#!/usr/bin/env python3
"""BaseOS 1-bit wireframe mockups. 1280x720, no antialiasing."""
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
MENUBAR_H = 32
TITLE_H = 28
INFO_H = 22
ICON_S = 32
CLOSE_S = 13
TASKBAR_H = 30
CHAR_H = 16
BLACK, WHITE = 0, 255
OUT = "/workspace/base-os/design"

# Disable TTF antialiasing
ImageDraw.ImageDraw.fontmode = "1"

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def font(size, bold=False):
    return ImageFont.truetype(FONT_BOLD if bold else FONT_PATH, size)


F11 = font(11, True)
F12 = font(12, True)
F13 = font(13, True)
F14 = font(14, True)
F16 = font(16, True)
F18 = font(18, True)
F22 = font(22, True)
F28 = font(28, True)
F36 = font(36, True)


def new_screen(fill=WHITE):
    return Image.new("L", (W, H), fill)


def to_1bit(im):
    return im.point(lambda p: 0 if p < 128 else 255, "1")


def save(im, name):
    path = f"{OUT}/{name}"
    bw = to_1bit(im)
    bw.save(path, "PNG")
    return path, bw.size


def text_size(draw, text, fnt):
    b = draw.textbbox((0, 0), text, font=fnt)
    return b[2] - b[0], b[3] - b[1]


def draw_text(draw, xy, text, fnt, fill=BLACK, anchor="lt"):
    draw.text(xy, text, font=fnt, fill=fill, anchor=anchor)


def dither_rect(im, box, phase=0):
    """50% checkerboard dither (Classic Mac desktop)."""
    x0, y0, x1, y1 = [int(v) for v in box]
    x0 = max(0, x0)
    y0 = max(0, y0)
    x1 = min(im.size[0], x1)
    y1 = min(im.size[1], y1)
    if x1 <= x0 or y1 <= y0:
        return
    px = im.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            if ((x + y + phase) & 1) == 0:
                px[x, y] = BLACK
            else:
                px[x, y] = WHITE


def hatch_rect(im, box, kind):
    """1-bit fill patterns so 8 theme swatches stay distinct."""
    x0, y0, x1, y1 = [int(v) for v in box]
    px = im.load()
    w_im, h_im = im.size
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w_im, x1), min(h_im, y1)
    for y in range(y0, y1):
        for x in range(x0, x1):
            v = WHITE
            if kind == "black":
                v = BLACK
            elif kind == "white":
                v = WHITE
            elif kind == "dither":
                v = BLACK if ((x + y) & 1) == 0 else WHITE
            elif kind == "dense":
                v = BLACK if (x % 2 == 0 or y % 2 == 0) else WHITE
            elif kind == "sparse":
                v = BLACK if (x % 3 == 0 and y % 3 == 0) else WHITE
            elif kind == "hlin":
                v = BLACK if (y % 3) == 0 else WHITE
            elif kind == "vlin":
                v = BLACK if (x % 3) == 0 else WHITE
            elif kind == "diag":
                v = BLACK if ((x + y) % 4) < 2 else WHITE
            elif kind == "cross":
                v = BLACK if (x % 4 == 0 or y % 4 == 0) else WHITE
            elif kind == "brick":
                v = BLACK if (y % 4 == 0) or ((x + (4 if (y // 4) % 2 else 0)) % 8 == 0) else WHITE
            px[x, y] = v


def rect(draw, box, outline=BLACK, fill=None, width=1):
    draw.rectangle(box, outline=outline, fill=fill, width=width)


# ----- chrome -----

def draw_menubar(im, time_str="12:00"):
    d = ImageDraw.Draw(im)
    rect(d, (0, 0, W - 1, MENUBAR_H - 1), outline=BLACK, fill=WHITE)
    d.line((0, MENUBAR_H - 1, W - 1, MENUBAR_H - 1), fill=BLACK, width=1)
    items = [("BaseOS", 12), ("File", 92), ("Edit", 148), ("Special", 204)]
    for name, x in items:
        draw_text(d, (x, MENUBAR_H // 2), name, F13, BLACK, "lm")
    tw, _ = text_size(d, time_str, F13)
    draw_text(d, (W - 16 - tw, MENUBAR_H // 2), time_str, F13, BLACK, "lm")


def draw_floppy(d, x, y, s=ICON_S):
    """Crude 1-bit floppy."""
    rect(d, (x, y, x + s - 1, y + s - 1), BLACK, WHITE)
    # shutter
    rect(d, (x + 6, y + 3, x + s - 7, y + 12), BLACK, WHITE)
    d.rectangle((x + s - 12, y + 5, x + s - 9, y + 10), fill=BLACK)
    # label area
    rect(d, (x + 4, y + 16, x + s - 5, y + s - 4), BLACK, WHITE)
    # spine
    d.line((x, y + 2, x + s - 1, y + 2), fill=BLACK)


def draw_trash(d, x, y, s=ICON_S):
    """Crude 1-bit trash can."""
    # lid
    d.line((x + 6, y + 6, x + s - 7, y + 6), fill=BLACK)
    d.line((x + 14, y + 2, x + 14, y + 6), fill=BLACK)  # handle
    d.line((x + 14, y + 2, x + 18, y + 2), fill=BLACK)
    d.line((x + 4, y + 7, x + s - 5, y + 7), fill=BLACK)
    # can body (slight taper)
    d.polygon(
        [
            (x + 6, y + 8),
            (x + s - 7, y + 8),
            (x + s - 10, y + s - 2),
            (x + 9, y + s - 2),
        ],
        outline=BLACK,
        fill=WHITE,
    )
    # ridges
    for dx in (11, 16, 21):
        d.line((x + dx, y + 11, x + dx, y + s - 6), fill=BLACK)


def draw_desktop_icons(im):
    d = ImageDraw.Draw(im)
    disk_x = W - ICON_S - 24  # 1224
    disk_y = MENUBAR_H + 16  # 48
    draw_floppy(d, disk_x, disk_y)
    draw_text(d, (disk_x + ICON_S // 2, disk_y + ICON_S + 2), "BaseOS", F11, BLACK, "mt")

    trash_x = W - ICON_S - 24  # 1224
    trash_y = H - TASKBAR_H - ICON_S - CHAR_H - 12  # 630
    draw_trash(d, trash_x, trash_y)
    draw_text(d, (trash_x + ICON_S // 2, trash_y + ICON_S + 2), "Trash", F11, BLACK, "mt")


def draw_taskbar(im, apps, active=None, label=True):
    """apps: list of names. active: highlighted name."""
    d = ImageDraw.Draw(im)
    ty = H - TASKBAR_H
    rect(d, (0, ty, W - 1, H - 1), BLACK, WHITE)
    d.line((0, ty, W - 1, ty), fill=BLACK, width=1)

    x = 8
    for name in apps:
        # mini icon 16x16 + 6 gap + name + 16 pad
        tw, _ = text_size(d, name, F13)
        slot_w = 16 + 6 + tw + 16
        inverted = name == active
        x0, y0 = x, ty + 4
        x1, y1 = x + slot_w, ty + TASKBAR_H - 5
        if inverted:
            d.rectangle((x0, y0, x1, y1), fill=BLACK, outline=BLACK)
            # mini icon inverted
            d.rectangle((x0 + 4, y0 + 3, x0 + 16, y1 - 3), outline=WHITE)
            draw_text(d, (x0 + 22, (y0 + y1) // 2), name, F13, WHITE, "lm")
        else:
            d.rectangle((x0, y0, x1, y1), fill=WHITE, outline=BLACK)
            d.rectangle((x0 + 4, y0 + 3, x0 + 16, y1 - 3), outline=BLACK)
            draw_text(d, (x0 + 22, (y0 + y1) // 2), name, F13, BLACK, "lm")
        x = x1 + 6

    if label:
        draw_text(d, (W - 12, ty + TASKBAR_H // 2), "running apps", F12, BLACK, "rm")


def desktop(apps=None, active=None, with_taskbar=True):
    im = new_screen(WHITE)
    dither_rect(im, (0, MENUBAR_H, W, H))
    draw_menubar(im)
    draw_desktop_icons(im)
    if with_taskbar:
        draw_taskbar(im, apps or [], active=active, label=True)
    return im


# ----- windows -----

def title_stripes(d, x, y, w, plaque_x, plaque_x2):
    """Horizontal 1px stripes, skipped over the title plaque."""
    for i in range(4, TITLE_H - 4, 2):
        yy = y + i
        if plaque_x > x + 22:
            d.line((x + 22, yy, plaque_x - 4, yy), fill=BLACK)
        if plaque_x2 + 4 < x + w - 6:
            d.line((plaque_x2 + 4, yy, x + w - 6, yy), fill=BLACK)


def draw_close(d, wx, wy):
    cx = wx + 6
    cy = wy + (TITLE_H - CLOSE_S) // 2
    rect(d, (cx, cy, cx + CLOSE_S - 1, cy + CLOSE_S - 1), BLACK, WHITE)
    # inner mark
    rect(d, (cx + 2, cy + 2, cx + CLOSE_S - 3, cy + CLOSE_S - 3), BLACK, WHITE)


def draw_window(im, x, y, w, h, title, active=True, info=None):
    d = ImageDraw.Draw(im)
    # body
    rect(d, (x, y, x + w - 1, y + h - 1), BLACK, WHITE, width=1)
    # title bar fill
    # title bar: always white fill. Active gets stripes; inactive stays plain.
    d.rectangle((x + 1, y + 1, x + w - 2, y + TITLE_H - 1), fill=WHITE)

    # title plaque (always white + black text)
    tw, th = text_size(d, title, F13)
    pad = 10
    px1 = x + (w - tw) // 2 - pad
    px2 = px1 + tw + pad * 2
    py1 = y + 5
    py2 = y + TITLE_H - 6
    d.rectangle((px1, py1, px2, py2), fill=WHITE, outline=None)
    if active:
        title_stripes(d, x, y, w, px1, px2)
    draw_text(d, (x + w // 2, y + TITLE_H // 2), title, F13, BLACK, "mm")

    draw_close(d, x, y)
    # rule under title
    d.line((x, y + TITLE_H, x + w - 1, y + TITLE_H), fill=BLACK)

    info_h = 0
    if info is not None:
        iy = y + TITLE_H
        d.rectangle((x + 1, iy + 1, x + w - 2, iy + INFO_H - 1), fill=WHITE)
        draw_text(d, (x + 10, iy + INFO_H // 2), info, F12, BLACK, "lm")
        d.line((x, iy + INFO_H, x + w - 1, iy + INFO_H), fill=BLACK)
        info_h = INFO_H

    return y + TITLE_H + info_h  # body top


def folder_icon(d, x, y, s=14):
    d.polygon(
        [(x, y + 4), (x + 5, y + 4), (x + 7, y), (x + s, y), (x + s, y + s - 2), (x, y + s - 2)],
        outline=BLACK,
        fill=WHITE,
    )


def app_icon(d, x, y, s=14):
    rect(d, (x, y, x + s, y + s - 2), BLACK, WHITE)
    d.line((x + 3, y + 4, x + s - 3, y + 4), fill=BLACK)
    d.line((x + 3, y + 8, x + s - 5, y + 8), fill=BLACK)


def draw_files_window(im, x, y, w, h, active=False):
    body_top = draw_window(im, x, y, w, h, "Files", active=active, info="/")
    d = ImageDraw.Draw(im)
    rows = [
        ("folder", "Documents"),
        ("folder", "Pictures"),
        ("app", "Calculator"),
        ("app", "Paint"),
        ("app", "Snake"),
        ("app", "Wordle"),
        ("app", "Terminal"),
        ("app", "Todo"),
        ("app", "Image Viewer"),
        ("file", "readme.txt"),
    ]
    ROW = 24
    for i, (kind, name) in enumerate(rows):
        ry = body_top + 8 + i * ROW
        if ry + ROW > y + h - 8:
            break
        if kind == "folder":
            folder_icon(d, x + 16, ry + 2)
        else:
            app_icon(d, x + 16, ry + 2)
        draw_text(d, (x + 38, ry + 8), name, F13, BLACK, "lm")
    # scrollbar
    sb = 16
    rect(d, (x + w - sb - 1, body_top, x + w - 1, y + h - 1), BLACK, WHITE)
    # arrows
    d.polygon(
        [
            (x + w - sb // 2 - 1, body_top + 6),
            (x + w - 5, body_top + 14),
            (x + w - sb + 3, body_top + 14),
        ],
        outline=BLACK,
        fill=BLACK,
    )


def draw_calc_window(im, x, y, w, h, active=True):
    body_top = draw_window(im, x, y, w, h, "Calculator", active=active)
    d = ImageDraw.Draw(im)
    # display
    dx0, dy0 = x + 16, body_top + 10
    dx1, dy1 = x + w - 16, dy0 + 36
    rect(d, (dx0, dy0, dx1, dy1), BLACK, WHITE)
    draw_text(d, (dx1 - 10, (dy0 + dy1) // 2), "0", F18, BLACK, "rm")

    keys = [
        ["C", "", "", ""],
        ["7", "8", "9", "/"],
        ["4", "5", "6", "*"],
        ["1", "2", "3", "-"],
        ["0", ".", "=", "+"],
    ]
    kw, kh, gap = 56, 50, 8
    ox, oy = x + 16, dy1 + 10
    for r, row in enumerate(keys):
        for c, lab in enumerate(row):
            if not lab:
                continue
            kx = ox + c * (kw + gap)
            ky = oy + r * (kh + gap)
            if ky + kh > y + h - 8:
                continue
            rect(d, (kx, ky, kx + kw - 1, ky + kh - 1), BLACK, WHITE)
            draw_text(d, (kx + kw // 2, ky + kh // 2), lab, F16, BLACK, "mm")


# ----- Kilroy 48-wide 1-bit from OVERNIGHT.md -----

KILROY = [
    ".................##.................##",
    "................#..#...............#..#",
    "................#..#...............#..#",
    "................#..#...............#..#",
    "...........#############################",
    "...........#...........................#",
    "............#........#####.............#",
    ".............#......#.....#...........#",
    "..............#....#..#.#..#.........#",
    "...............#...#..#.#..#........#",
    "................#...##...##........#",
    ".................#...............#",
    "..................###############",
    "........................##",
    ".......................#..#",
    "........................##",
]


def blit_kilroy(im, cx, wall_y, scale=2, color=BLACK):
    """Draw Kilroy so the wall row (index 4) sits on wall_y. cx is center x."""
    rows = KILROY
    max_w = max(len(r) for r in rows)
    # pad rows to max_w
    rows = [r + "." * (max_w - len(r)) for r in rows]
    wall_row = 4
    x0 = cx - (max_w * scale) // 2
    y0 = wall_y - wall_row * scale
    px = im.load()
    w_im, h_im = im.size
    for j, row in enumerate(rows):
        for i, ch in enumerate(row):
            if ch != "#":
                continue
            for dy in range(scale):
                for dx in range(scale):
                    xx, yy = x0 + i * scale + dx, y0 + j * scale + dy
                    if 0 <= xx < w_im and 0 <= yy < h_im:
                        px[xx, yy] = color


# ----- scenes -----

def make_desktop_taskbar():
    im = desktop(apps=["Files", "Calculator"], active=None)
    return save(im, "wf-desktop-taskbar.png")


def make_multitask():
    im = desktop(apps=["Files", "Calculator"], active="Calculator")
    # Files back-left, inactive
    draw_files_window(im, 80, 60, 720, 480, active=False)
    # Calculator front-right, active
    draw_calc_window(im, 740, 160, 280, 380, active=True)
    return save(im, "wf-multitask.png")


def make_settings():
    im = desktop(apps=["Settings"], active="Settings")
    ww, hh = 560, 300
    wx = (W - ww) // 2  # 360
    wy = MENUBAR_H + (H - MENUBAR_H - TASKBAR_H - hh) // 2  # 211
    body_top = draw_window(im, wx, wy, ww, hh, "Settings", active=True)
    d = ImageDraw.Draw(im)

    draw_text(d, (wx + 16, body_top + 12), "Desktop + title bar", F13, BLACK, "lt")

    # 8 swatches, 2x4. 56x40, gap 12. origin (wx+24, wy+TITLE_H+40)
    ox, oy = wx + 24, wy + TITLE_H + 40
    names = ["Classic", "Graphite", "Navy", "Pine", "Wine", "Paper", "Cyan", "Magenta"]
    fills = ["dither", "dense", "black", "vlin", "hlin", "white", "diag", "cross"]
    # title-strip patterns (top 8px of swatch) — title color stand-in
    strips = ["white", "dither", "vlin", "black", "hlin", "black", "black", "dense"]
    SW, SH, GAP = 56, 40, 12
    selected = 0
    for i, (name, fillp, strip) in enumerate(zip(names, fills, strips)):
        col, row = i % 4, i // 4
        sx = ox + col * (SW + GAP)
        sy = oy + row * (SH + 28)  # 12 gap + 16 name
        hatch_rect(im, (sx, sy, sx + SW, sy + SH), fillp)
        hatch_rect(im, (sx, sy, sx + SW, sy + 8), strip)
        d = ImageDraw.Draw(im)
        rect(d, (sx, sy, sx + SW - 1, sy + SH - 1), BLACK, None)
        # 8px title strip separator
        d.line((sx, sy + 8, sx + SW - 1, sy + 8), fill=BLACK)
        if i == selected:
            # 1px inset mark
            rect(d, (sx + 2, sy + 2, sx + SW - 3, sy + SH - 3), BLACK, None)
        draw_text(d, (sx + SW // 2, sy + SH + 4), name, F11, BLACK, "mt")
    return save(im, "wf-settings-themes.png")


def tool_doodle(d, kind, x, y, s):
    """Tiny 1-bit tool glyph inside a box."""
    m = 6
    x0, y0, x1, y1 = x + m, y + m, x + s - m, y + s - m
    cx, cy = (x + s // 2), (y + s // 2)
    if kind == "pencil":
        d.line((x0, y1, x1, y0), fill=BLACK, width=2)
        d.polygon([(x1 - 2, y0), (x1, y0), (x1, y0 + 2)], fill=BLACK)
    elif kind == "fill":
        # bucket
        d.polygon(
            [(cx - 6, cy - 2), (cx + 5, cy - 2), (cx + 3, cy + 7), (cx - 4, cy + 7)],
            outline=BLACK,
        )
        d.line((cx + 4, cy - 4, cx + 7, cy - 1), fill=BLACK)
        d.point((cx - 2, cy + 9), fill=BLACK)
        d.point((cx, cy + 10), fill=BLACK)
    elif kind == "line":
        d.line((x0, y1, x1, y0), fill=BLACK)
    elif kind == "rect":
        rect(d, (x0, y0 + 2, x1, y1 - 2), BLACK, None)
    elif kind == "ellipse":
        d.ellipse((x0, y0 + 2, x1, y1 - 2), outline=BLACK)
    elif kind == "text":
        draw_text(d, (cx, cy), "A", F16, BLACK, "mm")
    elif kind == "eraser":
        d.polygon(
            [(x0 + 2, cy + 2), (cx - 2, y0 + 2), (x1 - 2, y0 + 4), (cx + 4, y1 - 2)],
            outline=BLACK,
            fill=WHITE,
        )


def make_paint():
    im = desktop(apps=["Paint"], active="Paint")
    ww, hh = 720, 460
    wx, wy = 80, 48
    body_top = draw_window(im, wx, wy, ww, hh, "Paint", active=True)
    d = ImageDraw.Draw(im)

    # Left tool strip — labeled boxes
    tools = [
        ("pencil", "pencil"),
        ("fill", "fill"),
        ("line", "line"),
        ("rect", "rect"),
        ("ellipse", "ellipse"),
        ("text", "text"),
        ("eraser", "eraser"),
    ]
    CELL = 28
    GAP = 4
    STRIP_W = 28 + 8 + 70  # icon + label
    WELL_H = 28
    ox = wx + 8
    oy = body_top + 8
    selected_tool = 0
    for i, (kind, lab) in enumerate(tools):
        tx = ox
        ty = oy + i * (CELL + GAP)
        rect(d, (tx, ty, tx + CELL - 1, ty + CELL - 1), BLACK, WHITE)
        if i == selected_tool:
            rect(d, (tx + 2, ty + 2, tx + CELL - 3, ty + CELL - 3), BLACK, None)
        tool_doodle(d, kind, tx, ty, CELL)
        draw_text(d, (tx + CELL + 6, ty + CELL // 2), lab, F11, BLACK, "lm")

    # strip divider
    div_x = wx + STRIP_W
    d.line((div_x, body_top, div_x, wy + hh - WELL_H), fill=BLACK)

    # canvas (rest of body minus wells)
    canvas_box = (div_x + 1, body_top + 1, wx + ww - 2, wy + hh - WELL_H - 1)
    # white canvas, 1px inset already from window
    d.rectangle(canvas_box, fill=WHITE, outline=None)
    # faint inner inset
    rect(d, canvas_box, BLACK, None)
    cx = (canvas_box[0] + canvas_box[2]) // 2
    cy = (canvas_box[1] + canvas_box[3]) // 2
    draw_text(d, (cx, cy), "canvas", F14, BLACK, "mm")

    # bottom 12 color wells
    well_y = wy + hh - 24
    names_short = list(range(12))
    well_fills = [
        "black",
        "dense",
        "dither",
        "white",
        "hlin",
        "diag",
        "sparse",
        "vlin",
        "cross",
        "brick",
        "dense",
        "dither",
    ]
    wwll, hhll, wgap = 22, 18, 4
    wox = wx + 12
    selected_well = 0
    for i, fp in enumerate(well_fills):
        sx = wox + i * (wwll + wgap)
        sy = well_y
        hatch_rect(im, (sx, sy, sx + wwll, sy + hhll), fp)
        d = ImageDraw.Draw(im)
        rect(d, (sx, sy, sx + wwll - 1, sy + hhll - 1), BLACK, None)
        if i == selected_well:
            rect(d, (sx + 2, sy + 2, sx + wwll - 3, sy + hhll - 3), WHITE if fp == "black" else BLACK, None)
    # well strip top rule
    d.line((wx, wy + hh - WELL_H, wx + ww - 1, wy + hh - WELL_H), fill=BLACK)
    return save(im, "wf-paint.png")


def make_boot():
    im = new_screen(WHITE)
    d = ImageDraw.Draw(im)
    cy = 360
    # BaseOS centered above the line
    draw_text(d, (W // 2, cy - 36), "BaseOS", F36, BLACK, "mm")
    # wall line 320px, 2px thick
    d.rectangle((480, 360, 800, 361), fill=BLACK)
    blit_kilroy(im, W // 2, 360, scale=3, color=BLACK)
    return save(im, "wf-boot.png")


def main():
    results = [
        make_desktop_taskbar(),
        make_multitask(),
        make_settings(),
        make_paint(),
        make_boot(),
    ]
    for path, size in results:
        import os
        nbytes = os.path.getsize(path)
        print(f"{path}  {size[0]}x{size[1]}  {nbytes} bytes")


if __name__ == "__main__":
    main()
