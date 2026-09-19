#!/usr/bin/env python3
"""Crude labeled 1280x720 wireframes for BaseOS overnight spec."""
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
MENUBAR_H = 32
TITLE_H = 28
INFO_H = 22
TASKBAR_H = 30
CLOSE_S = 13
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
GRAY = (160, 160, 160)
LGRAY = (210, 210, 210)
DGRAY = (90, 90, 90)
INK = (0, 0, 0)
HINT = (40, 40, 40)

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONTM = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


def font(size, bold=False, mono=False):
    path = FONTM if mono else (FONTB if bold else FONT)
    return ImageFont.truetype(path, size)


F11 = font(11)
F12 = font(12)
F13 = font(13)
F14 = font(14, bold=True)
F16 = font(16, bold=True)
F18 = font(18, bold=True)
F22 = font(22, bold=True)
F28 = font(28, bold=True)
F11M = font(11, mono=True)
F13M = font(13, mono=True)


def new_img(bg=WHITE):
    im = Image.new("RGB", (W, H), bg)
    dr = ImageDraw.Draw(im)
    return im, dr


def text_w(dr, s, f):
    b = dr.textbbox((0, 0), s, font=f)
    return b[2] - b[0]


def text_h(dr, s, f):
    b = dr.textbbox((0, 0), s, font=f)
    return b[3] - b[1]


def dither(dr, x, y, w, h, a=BLACK, b=LGRAY, step=2):
    dr.rectangle([x, y, x + w - 1, y + h - 1], fill=b)
    for yy in range(y, y + h, step):
        for xx in range(x + ((yy - y) // step % 2) * step, x + w, step * 2):
            dr.point((xx, yy), fill=a)


def hline(dr, x0, x1, y, col=BLACK, thick=1):
    dr.rectangle([x0, y, x1, y + thick - 1], fill=col)


def vline(dr, x, y0, y1, col=BLACK, thick=1):
    dr.rectangle([x, y0, x + thick - 1, y1], fill=col)


def rect(dr, x, y, w, h, fill=WHITE, outline=BLACK, width=1):
    dr.rectangle([x, y, x + w - 1, y + h - 1], fill=fill, outline=outline, width=width)


def label(dr, x, y, s, f=F12, fill=HINT):
    dr.text((x, y), s, font=f, fill=fill)


def callout(dr, tx, ty, text, ax, ay, side="left"):
    """Arrow from text to (ax,ay)."""
    f = F12
    tw = text_w(dr, text, f)
    th = 16
    pad = 4
    if side == "left":
        bx = tx
        by = ty
        rect(dr, bx, by, tw + pad * 2, th + 4, fill=WHITE, outline=BLACK)
        dr.text((bx + pad, by + 2), text, font=f, fill=BLACK)
        dr.line([(bx + tw + pad * 2, by + th // 2), (ax, ay)], fill=BLACK, width=1)
        dr.polygon([(ax, ay), (ax - 6, ay - 4), (ax - 6, ay + 4)], fill=BLACK)
    elif side == "right":
        bx = tx
        by = ty
        rect(dr, bx, by, tw + pad * 2, th + 4, fill=WHITE, outline=BLACK)
        dr.text((bx + pad, by + 2), text, font=f, fill=BLACK)
        dr.line([(bx, by + th // 2), (ax, ay)], fill=BLACK, width=1)
        dr.polygon([(ax, ay), (ax + 6, ay - 4), (ax + 6, ay + 4)], fill=BLACK)
    elif side == "down":
        bx = tx
        by = ty
        rect(dr, bx, by, tw + pad * 2, th + 4, fill=WHITE, outline=BLACK)
        dr.text((bx + pad, by + 2), text, font=f, fill=BLACK)
        cx = bx + (tw + pad * 2) // 2
        dr.line([(cx, by + th + 4), (ax, ay)], fill=BLACK, width=1)
        dr.polygon([(ax, ay), (ax - 4, ay - 6), (ax + 4, ay - 6)], fill=BLACK)
    else:  # up
        bx = tx
        by = ty
        rect(dr, bx, by, tw + pad * 2, th + 4, fill=WHITE, outline=BLACK)
        dr.text((bx + pad, by + 2), text, font=f, fill=BLACK)
        cx = bx + (tw + pad * 2) // 2
        dr.line([(cx, by), (ax, ay)], fill=BLACK, width=1)
        dr.polygon([(ax, ay), (ax - 4, ay + 6), (ax + 4, ay + 6)], fill=BLACK)


def draw_close(dr, wx, wy):
    cx = wx + 6
    cy = wy + (TITLE_H - CLOSE_S) // 2
    rect(dr, cx, cy, CLOSE_S, CLOSE_S, fill=WHITE, outline=BLACK)
    # inner box
    rect(dr, cx + 1, cy + 1, CLOSE_S - 2, CLOSE_S - 2, fill=WHITE, outline=BLACK)
    return cx, cy


def draw_stripes(dr, x0, x1, y, col=BLACK):
    y0 = y + 4
    y1 = y + TITLE_H - 4
    for yy in range(y0, y1 + 1, 2):
        hline(dr, x0, x1, yy, col)


def draw_window(dr, x, y, w, h, title, stripes=True, info=None, fill=WHITE):
    # drop shadow
    dr.rectangle([x + 3, y + 3, x + w + 2, y + h + 2], outline=BLACK)
    rect(dr, x, y, w, h, fill=fill, outline=BLACK)
    # title
    if stripes:
        draw_stripes(dr, x + CLOSE_S + 10, x + w - 6, y)
    else:
        dr.rectangle([x + 1, y + 1, x + w - 2, y + TITLE_H - 1], fill=LGRAY)
        hline(dr, x, x + w - 1, y + TITLE_H, BLACK)
    draw_close(dr, x, y)
    vline(dr, x + CLOSE_S + 8, y + 1, y + TITLE_H - 1)
    # title plaque
    tw = text_w(dr, title, F13) + 12
    tx = x + (w - tw) // 2
    if tx < x + CLOSE_S + 16:
        tx = x + CLOSE_S + 16
    rect(dr, tx, y + 4, tw, TITLE_H - 8, fill=WHITE, outline=None)
    dr.text((tx + 6, y + 6), title, font=F13, fill=BLACK)
    hline(dr, x, x + w - 1, y + TITLE_H, BLACK)
    top = TITLE_H
    if info is not None:
        dr.text((x + 8, y + TITLE_H + 3), info, font=F12, fill=BLACK)
        hline(dr, x, x + w - 1, y + TITLE_H + INFO_H, BLACK)
        top = TITLE_H + INFO_H
    return top


def draw_menubar(dr, clock="3:45 AM"):
    rect(dr, 0, 0, W, MENUBAR_H, fill=WHITE, outline=None)
    hline(dr, 0, W - 1, MENUBAR_H - 1, BLACK)
    names = ["BaseOS", "File", "Edit", "Special"]
    x = 16
    for n in names:
        tw = text_w(dr, n, F13) + 16
        dr.text((x + 8, 8), n, font=F13, fill=BLACK)
        x += tw
    cw = text_w(dr, clock, F13)
    dr.text((W - cw - 16, 8), clock, font=F13, fill=BLACK)


def draw_taskbar(dr, apps, active=0):
    y = H - TASKBAR_H
    rect(dr, 0, y, W, TASKBAR_H, fill=WHITE, outline=None)
    hline(dr, 0, W - 1, y, BLACK)
    x = 8
    for i, name in enumerate(apps):
        tw = text_w(dr, name, F12)
        sw = 16 + 6 + tw + 16
        if i == active:
            rect(dr, x, y + 4, sw, TASKBAR_H - 8, fill=BLACK, outline=BLACK)
            # mini icon
            rect(dr, x + 6, y + 8, 14, 14, fill=WHITE, outline=WHITE)
            dr.text((x + 24, y + 7), name, font=F12, fill=WHITE)
        else:
            rect(dr, x, y + 4, sw, TASKBAR_H - 8, fill=WHITE, outline=BLACK)
            rect(dr, x + 6, y + 8, 14, 14, fill=WHITE, outline=BLACK)
            dr.text((x + 24, y + 7), name, font=F12, fill=BLACK)
        x += sw + 6


def draw_disk_icon(dr, x, y, selected=False):
    # 32x32 floppy
    rect(dr, x, y, 32, 32, fill=WHITE, outline=BLACK)
    rect(dr, x + 10, y, 12, 8, fill=LGRAY, outline=BLACK)
    rect(dr, x + 6, y + 14, 20, 14, fill=WHITE, outline=BLACK)
    tw = text_w(dr, "BaseOS", F11)
    lx = x + 16 - tw // 2
    ly = y + 34
    rect(dr, lx - 3, ly - 1, tw + 6, 14, fill=BLACK if selected else WHITE, outline=BLACK)
    dr.text((lx, ly), "BaseOS", font=F11, fill=WHITE if selected else BLACK)


def draw_trash_icon(dr, x, y):
    # can
    dr.polygon(
        [(x + 8, y + 6), (x + 24, y + 6), (x + 22, y + 30), (x + 10, y + 30)],
        outline=BLACK,
        fill=WHITE,
    )
    hline(dr, x + 4, x + 28, y + 6, BLACK)
    hline(dr, x + 10, x + 22, y + 2, BLACK)
    for i in range(3):
        vline(dr, x + 12 + i * 4, y + 10, y + 26, BLACK)
    tw = text_w(dr, "Trash", F11)
    lx = x + 16 - tw // 2
    ly = y + 34
    rect(dr, lx - 3, ly - 1, tw + 6, 14, fill=WHITE, outline=BLACK)
    dr.text((lx, ly), "Trash", font=F11, fill=BLACK)


def kilroy(dr, cx, wall_y, scale=2):
    """Classic peeking Kilroy. wall_y is the wall line. cx is center."""
    # wall
    hline(dr, cx - 80 * scale // 2, cx + 80 * scale // 2, wall_y, BLACK, thick=2)
    # fingers / hang
    # eyes
    def oval(x, y, w, h):
        dr.ellipse([x, y, x + w, y + h], outline=BLACK, fill=WHITE)

    # head blob above wall
    # left eye
    ex = cx - 18 * scale
    ey = wall_y - 14 * scale
    oval(ex, ey, 10 * scale, 10 * scale)
    oval(cx + 8 * scale, ey, 10 * scale, 10 * scale)
    # pupils
    dr.ellipse([ex + 3 * scale, ey + 3 * scale, ex + 7 * scale, ey + 7 * scale], fill=BLACK)
    dr.ellipse(
        [cx + 8 * scale + 3 * scale, ey + 3 * scale, cx + 8 * scale + 7 * scale, ey + 7 * scale],
        fill=BLACK,
    )
    # nose hanging over wall
    nx = cx - 6 * scale
    ny = wall_y - 2 * scale
    dr.polygon(
        [
            (cx, wall_y - 4 * scale),
            (cx - 8 * scale, wall_y + 10 * scale),
            (cx + 8 * scale, wall_y + 10 * scale),
        ],
        outline=BLACK,
        fill=WHITE,
    )
    # nostrils
    dr.point((cx - 2 * scale, wall_y + 6 * scale), fill=BLACK)
    dr.point((cx + 2 * scale, wall_y + 6 * scale), fill=BLACK)
    # hair / ridge connecting eyes above wall
    dr.arc(
        [cx - 22 * scale, wall_y - 18 * scale, cx + 22 * scale, wall_y + 2 * scale],
        200,
        340,
        fill=BLACK,
        width=2,
    )


# ---------------------------------------------------------------------------
# 1. desktop + taskbar
# ---------------------------------------------------------------------------
def wf_desktop():
    im, dr = new_img()
    dither(dr, 0, 0, W, H, BLACK, (230, 230, 230), step=3)
    draw_menubar(dr)
    # icons
    disk_x, disk_y = W - 32 - 24, MENUBAR_H + 16
    trash_x, trash_y = W - 32 - 24, H - TASKBAR_H - 32 - 16 - 12
    draw_disk_icon(dr, disk_x, disk_y)
    draw_trash_icon(dr, trash_x, trash_y)
    draw_taskbar(dr, ["Calculator", "Files"], active=0)

    # watermark title
    label(dr, 16, 44, "wf-desktop-taskbar  1280x720", F14, BLACK)

    callout(dr, 40, 80, "menu bar 32px  black/white  clock right", 200, 16, "left")
    callout(dr, 700, 80, "floppy 32px  x=1224 y=48", disk_x, disk_y + 16, "right")
    callout(
        dr,
        640,
        520,
        "Trash ABOVE taskbar  y=630  (not covered)",
        trash_x,
        trash_y + 16,
        "right",
    )
    callout(
        dr,
        40,
        640,
        "taskbar 30px  running apps only  no Start  click focuses",
        80,
        H - 15,
        "left",
    )
    callout(dr, 200, 600, "slot: 16px icon + name  active inverted", 70, H - 16, "left")
    # gap marker between trash and bar
    gap_y0 = trash_y + 32 + 16
    gap_y1 = H - TASKBAR_H
    vline(dr, trash_x - 8, gap_y0, gap_y1 - 1, BLACK)
    dr.text((trash_x - 120, (gap_y0 + gap_y1) // 2 - 8), "gap ~12px", font=F11, fill=BLACK)
    im.save("/workspace/base-os/design/wf-desktop-taskbar.png")
    print("wrote wf-desktop-taskbar.png")


# ---------------------------------------------------------------------------
# 2. settings themes
# ---------------------------------------------------------------------------
def wf_settings():
    im, dr = new_img()
    dither(dr, 0, 0, W, H, BLACK, (230, 230, 230), step=3)
    draw_menubar(dr)
    draw_taskbar(dr, ["Settings"], active=0)
    disk_x, disk_y = W - 32 - 24, MENUBAR_H + 16
    trash_x, trash_y = W - 32 - 24, H - TASKBAR_H - 32 - 16 - 12
    draw_disk_icon(dr, disk_x, disk_y)
    draw_trash_icon(dr, trash_x, trash_y)

    ww, wh = 560, 300
    wx = (W - ww) // 2
    wy = MENUBAR_H + (H - MENUBAR_H - TASKBAR_H - wh) // 2
    draw_window(dr, wx, wy, ww, wh, "Settings", stripes=True)
    dr.text((wx + 16, wy + TITLE_H + 12), "Desktop theme", font=F13, fill=BLACK)

    names = ["Classic", "Graphite", "Navy", "Pine", "Wine", "Paper", "Cyan", "Magenta"]
    fills = [
        None,  # dither
        (64, 64, 64),
        (32, 48, 96),
        (58, 90, 64),
        (90, 48, 64),
        (192, 192, 192),
        (32, 144, 168),
        (144, 48, 160),
    ]
    titles = [
        WHITE,
        (128, 128, 128),
        (32, 64, 168),
        (46, 139, 58),
        (196, 30, 58),
        BLACK,
        (32, 48, 96),
        (139, 90, 43),
    ]
    ox, oy = wx + 24, wy + TITLE_H + 40
    sw, sh, gapx, gapy = 56, 40, 12, 28
    for i, name in enumerate(names):
        col, row = i % 4, i // 4
        sx = ox + col * (sw + gapx + 40)  # extra for name width
        # tighter: 4 cols in 560-48=512, (512-4*56)/3 = 96? let's pack:
        sx = ox + col * (56 + 72)
        sy = oy + row * (40 + 28)
        if fills[i] is None:
            dither(dr, sx, sy, sw, sh, BLACK, WHITE, step=2)
            rect(dr, sx, sy, sw, sh, fill=None, outline=BLACK)
        else:
            rect(dr, sx, sy, sw, sh, fill=fills[i], outline=BLACK)
        # title strip
        dr.rectangle([sx + 1, sy + 1, sx + sw - 2, sy + 8], fill=titles[i])
        if i == 0:
            # 1px inset mark
            rect(dr, sx + 2, sy + 2, sw - 4, sh - 4, fill=None, outline=BLACK)
        nw = text_w(dr, name, F11)
        dr.text((sx + (sw - nw) // 2, sy + sh + 4), name, font=F11, fill=BLACK)

    label(dr, 16, 44, "wf-settings-themes  1280x720", F14, BLACK)
    callout(dr, 16, 100, "8 swatches  56x40  2 rows x 4", wx + 24, oy + 20, "left")
    callout(
        dr,
        16,
        160,
        "current = 1px inset mark  (Classic selected)",
        wx + 40,
        oy + 20,
        "left",
    )
    callout(
        dr,
        16,
        220,
        "click applies + writes /prefs/theme",
        wx + 80,
        oy + 80,
        "left",
    )
    callout(
        dr,
        900,
        120,
        "menus stay black/white",
        80,
        16,
        "right",
    )
    callout(
        dr,
        820,
        500,
        "mini preview: desktop fill + 8px title strip",
        wx + 400,
        oy + 20,
        "right",
    )
    im.save("/workspace/base-os/design/wf-settings-themes.png")
    print("wrote wf-settings-themes.png")


# ---------------------------------------------------------------------------
# 3. paint
# ---------------------------------------------------------------------------
def wf_paint():
    im, dr = new_img()
    dither(dr, 0, 0, W, H, BLACK, (230, 230, 230), step=3)
    draw_menubar(dr)
    draw_taskbar(dr, ["Paint"], active=0)
    disk_x, disk_y = W - 32 - 24, MENUBAR_H + 16
    trash_x, trash_y = W - 32 - 24, H - TASKBAR_H - 32 - 16 - 12
    draw_disk_icon(dr, disk_x, disk_y)
    draw_trash_icon(dr, trash_x, trash_y)

    ww, wh = 720, 460
    wx, wy = 80, 48
    draw_window(dr, wx, wy, ww, wh, "Paint", stripes=True)

    # tool strip
    tools = ["P", "F", "L", "R", "E", "T", "X"]
    tnames = ["pencil", "fill", "line", "rect", "ellipse", "text", "eraser"]
    ty = wy + TITLE_H + 4
    tx = wx + 8
    for i, (g, n) in enumerate(zip(tools, tnames)):
        bx = tx + i * 32
        fill = BLACK if i == 0 else WHITE
        fg = WHITE if i == 0 else BLACK
        rect(dr, bx, ty, 28, 28, fill=fill, outline=BLACK)
        if i == 0:
            rect(dr, bx + 2, ty + 2, 24, 24, fill=None, outline=WHITE)
        tw = text_w(dr, g, F14)
        dr.text((bx + (28 - tw) // 2, ty + 5), g, font=F14, fill=fg)
    hline(dr, wx, wx + ww - 1, wy + TITLE_H + 36, BLACK)

    # canvas 3x 160x100 = 480x300 centered
    cw, ch = 480, 300
    cx = wx + (ww - cw) // 2
    cy = wy + TITLE_H + 36 + 8
    rect(dr, cx, cy, cw, ch, fill=WHITE, outline=BLACK)
    # fake doodle
    dr.line([(cx + 40, cy + 40), (cx + 200, cy + 120)], fill=BLACK, width=2)
    dr.ellipse([cx + 220, cy + 60, cx + 340, cy + 180], outline=BLACK, width=2)
    dr.rectangle([cx + 80, cy + 160, cx + 180, cy + 240], outline=BLACK, width=2)
    dr.text((cx + 250, cy + 220), "hello", font=F16, fill=BLACK)
    dr.text((cx + 8, cy + 8), "canvas 160x100 logical  shown 3x = 480x300", font=F11, fill=DGRAY)

    # color well bottom
    well_y = wy + wh - 28
    hline(dr, wx, wx + ww - 1, well_y, BLACK)
    colors = [
        BLACK,
        (64, 64, 64),
        (128, 128, 128),
        WHITE,
        (196, 30, 58),
        (224, 112, 32),
        (224, 192, 64),
        (46, 139, 58),
        (32, 144, 168),
        (32, 64, 168),
        (144, 48, 160),
        (139, 90, 43),
    ]
    ox = wx + 12
    for i, c in enumerate(colors):
        sx = ox + i * 26
        rect(dr, sx, well_y + 5, 22, 18, fill=c, outline=BLACK)
        if i == 0:
            rect(dr, sx + 2, well_y + 7, 18, 14, fill=None, outline=WHITE)

    label(dr, 16, 14, "", F14)  # noop
    # put filename label on desktop left of window
    dr.rectangle([16, 520, 380, 545], fill=WHITE, outline=BLACK)
    dr.text((20, 524), "wf-paint  1280x720", font=F14, fill=BLACK)

    callout(dr, 16, 100, "tools 28x28  top strip 36px  selected inset", wx + 20, ty + 14, "left")
    callout(dr, 820, 200, "File->Save  untitled.pbm  BOS1 header  into Pictures", cx + cw, cy + 40, "right")
    callout(dr, 16, 560, "12-color well  22x18  bottom 28px", wx + 20, well_y + 14, "left")
    callout(dr, 820, 400, "no layers  pencil/fill/line/rect/ellipse/text/eraser", cx + cw, cy + 160, "right")
    im.save("/workspace/base-os/design/wf-paint.png")
    print("wrote wf-paint.png")


# ---------------------------------------------------------------------------
# 4. boot splash
# ---------------------------------------------------------------------------
def wf_boot():
    im, dr = new_img(WHITE)
    # centered BaseOS
    s = "BaseOS"
    tw = text_w(dr, s, F28)
    tx = (W - tw) // 2
    ty = 300
    dr.text((tx, ty), s, font=F28, fill=BLACK)

    wall_y = 400
    hline(dr, 480, 800, wall_y, BLACK, thick=2)
    kilroy(dr, W // 2, wall_y, scale=2)

    # small caption under
    cap = "2.5 seconds, then desktop"
    cw = text_w(dr, cap, F13)
    dr.text(((W - cw) // 2, 520), cap, font=F13, fill=DGRAY)

    label(dr, 16, 16, "wf-boot  1280x720", F14, BLACK)
    callout(dr, 40, 200, "centered Chicago-style title", tx, ty + 10, "left")
    callout(dr, 40, 440, "Kilroy peeking over a line  1-bit doodle", W // 2 - 40, wall_y, "left")
    callout(dr, 860, 200, "white fill  no logo festival", tx + tw, ty + 10, "right")
    callout(dr, 860, 500, "hold ~150 VGA frames  no keypress", 700, 530, "right")
    im.save("/workspace/base-os/design/wf-boot.png")
    print("wrote wf-boot.png")


# ---------------------------------------------------------------------------
# 5. multitask two windows + taskbar
# ---------------------------------------------------------------------------
def wf_multitask():
    im, dr = new_img()
    dither(dr, 0, 0, W, H, BLACK, (230, 230, 230), step=3)
    draw_menubar(dr)
    disk_x, disk_y = W - 32 - 24, MENUBAR_H + 16
    trash_x, trash_y = W - 32 - 24, H - TASKBAR_H - 32 - 16 - 12
    draw_disk_icon(dr, disk_x, disk_y)
    draw_trash_icon(dr, trash_x, trash_y)

    # back window: Files, plain title, no stripes
    fx, fy, fw, fh = 80, 60, 720, 480
    # keep it from covering taskbar visually — 480+60=540, taskbar at 690, ok
    top = draw_window(dr, fx, fy, fw, fh, "BaseOS", stripes=False, info="/")
    # list rows
    rows = ["..", "docs", "Documents", "Pictures", "Calculator", "Paint", "Snake", "Wordle", "Terminal", "Todo", "Image Viewer", "readme.txt", "Hello"]
    # skip .. at root — show without ..
    rows = ["docs", "Documents", "Pictures", "Calculator", "Paint", "Snake", "Wordle", "Terminal", "Todo", "Image Viewer", "readme.txt", "Hello"]
    ly = fy + top + 8
    for i, name in enumerate(rows):
        yy = ly + i * 24
        if yy + 20 > fy + fh - 24:
            break
        # mini icon
        if name in ("docs", "Documents", "Pictures"):
            rect(dr, fx + 14, yy + 4, 12, 10, fill=WHITE, outline=BLACK)
            dr.rectangle([fx + 14, yy + 2, fx + 20, yy + 5], outline=BLACK)
        else:
            rect(dr, fx + 16, yy + 2, 10, 12, fill=WHITE, outline=BLACK)
        dr.text((fx + 36, yy + 4), name, font=F12, fill=BLACK)
    # fake scrollbar
    sbx = fx + fw - 20
    rect(dr, sbx, fy + top, 20, fh - top - 20, fill=WHITE, outline=BLACK)
    rect(dr, sbx, fy + fh - 20, 20, 20, fill=WHITE, outline=BLACK)  # grow

    # front window: Calculator, striped
    cw, ch = 280, 380
    cx, cy = 520, 140
    draw_window(dr, cx, cy, cw, ch, "Calculator", stripes=True)
    # display
    dx, dy, dw, dh = cx + 16, cy + TITLE_H + 10, 248, 36
    rect(dr, dx, dy, dw, dh, fill=WHITE, outline=BLACK)
    ds = "3.14"
    dr.text((dx + dw - 8 - text_w(dr, ds, F16), dy + 8), ds, font=F16, fill=BLACK)
    keys = [
        ("C", 0, 0),
        ("7", 1, 0), ("8", 1, 1), ("9", 1, 2), ("/", 1, 3),
        ("4", 2, 0), ("5", 2, 1), ("6", 2, 2), ("*", 2, 3),
        ("1", 3, 0), ("2", 3, 1), ("3", 3, 2), ("-", 3, 3),
        ("0", 4, 0), (".", 4, 1), ("=", 4, 2), ("+", 4, 3),
    ]
    kx, ky0 = cx + 16, dy + dh + 10
    KW, KH, GAP = 56, 50, 8
    for ch_, row, col in keys:
        if row == 0:
            ky = ky0
        else:
            ky = ky0 + KH + GAP + (row - 1) * (KH + GAP)
        bx = kx + col * (KW + GAP)
        rect(dr, bx, ky, KW, KH, fill=WHITE, outline=BLACK)
        tw = text_w(dr, ch_, F16)
        dr.text((bx + (KW - tw) // 2, ky + 14), ch_, font=F16, fill=BLACK)

    draw_taskbar(dr, ["Files", "Calculator"], active=1)

    dr.rectangle([16, 44, 420, 70], fill=WHITE, outline=BLACK)
    dr.text((20, 48), "wf-multitask  1280x720", font=F14, fill=BLACK)

    callout(dr, 16, 90, "back window: plain title  no stripes", fx + 80, fy + 14, "left")
    callout(dr, 860, 90, "front window: stripes  close box closes THIS app", cx + 140, cy + 14, "right")
    callout(dr, 16, 560, "click window or taskbar to focus", 200, H - 16, "left")
    callout(dr, 820, 560, "drag by title bar  click-hold, window follows", cx + 140, cy + 14, "right")
    im.save("/workspace/base-os/design/wf-multitask.png")
    print("wrote wf-multitask.png")


if __name__ == "__main__":
    wf_desktop()
    wf_settings()
    wf_paint()
    wf_boot()
    wf_multitask()
