"""Render the icon set into src/icons.h.

The icons are from Lucide (ISC license, see third_party/lucide/LICENSE). Get the
font with `npm pack lucide-static@1.57.0` and pass the extracted package folder:

    python3 tools/gen_icons.py /path/to/package [--sheet icons.png]

Icons are rendered at ICON_SIZE for toolbars, rows and controls. Glyphs are
drawn at 4x and box-filtered down, then stored as 16 coverage levels (4 bits
per pixel), the same scheme as the UI font, so drawing blends them over any
surface. The color application icons are a separate set (tools/icon_art.py).
Only the generated header is committed; normal builds do not need Pillow.
"""
import argparse
import json
import pathlib

from PIL import Image, ImageDraw, ImageFont

ICON_SIZE = 16
SS = 4
GAMMA = 0.85
ROOT = pathlib.Path(__file__).resolve().parents[1]

# (enum name, Lucide name). Order defines the enum values.
ICONS = [
    ("BACK", "arrow-left"), ("FORWARD", "arrow-right"), ("UP", "arrow-up"),
    ("RELOAD", "rotate-cw"), ("STOP", "x"), ("HOME", "house"),
    ("SAVE", "save"), ("SAVE_AS", "save-all"), ("DOWNLOAD", "download"),
    ("LINK", "link"), ("LIST", "list"), ("BOLD", "bold"), ("ITALIC", "italic"),
    ("UNDERLINE", "underline"), ("HEADING", "heading"), ("BODY", "pilcrow"),
    ("ALIGN_LEFT", "align-left"), ("ALIGN_CENTER", "align-center"),
    ("ALIGN_RIGHT", "align-right"), ("UNDO", "undo-2"), ("REDO", "redo-2"),
    ("FILE_TEXT", "file-text"), ("FILE_TYPE", "file-type"), ("SEARCH", "search"),
    ("REPLACE", "replace"), ("TABLE", "table"), ("EDIT", "pencil"),
    ("GO", "corner-down-left"), ("PLAY", "play"), ("PAUSE", "pause"),
    ("STOP_SQUARE", "square"), ("PREV", "skip-back"), ("NEXT", "skip-forward"),
    ("VOLUME", "volume-2"), ("PLUS", "plus"), ("MINUS", "minus"),
    ("FIT", "maximize"), ("ACTUAL", "scan"), ("CALENDAR", "calendar"),
    ("TRASH", "trash-2"), ("CHEVRON_LEFT", "chevron-left"),
    ("CHEVRON_RIGHT", "chevron-right"), ("CHEVRON_UP", "chevron-up"),
    ("CHEVRON_DOWN", "chevron-down"), ("FILTER", "funnel"), ("CLEAR", "circle-x"),
    ("REFRESH", "refresh-cw"), ("CHECK", "check"), ("FOLDER", "folder"),
    ("SETTINGS", "settings"), ("EYE", "eye"), ("EXPORT", "file-output"),
    ("SHEET", "sheet"), ("CALENDAR_PLUS", "calendar-plus"),
    ("TODAY", "calendar-days"), ("HASH", "hash"), ("PERCENT", "percent"),
    ("DOLLAR", "dollar-sign"), ("JUMP", "arrow-right-to-line"),
    ("FILE_PLUS", "file-plus"), ("ZOOM_IN", "zoom-in"), ("ZOOM_OUT", "zoom-out"),
    ("LOCATE", "locate"), ("CLOSE", "square-x"), ("COPY", "copy"),
    ("PASTE", "clipboard-paste"), ("CUT", "scissors"), ("LIST_ORDERED", "list-ordered"),
    # Application glyphs used as small marks (search results, Files rows).
    ("APP_FILES", "folder"), ("APP_EDITOR", "file-text"), ("APP_CALC", "calculator"),
    ("APP_PAINT", "paintbrush"), ("APP_VIEWER", "image"), ("APP_SNAKE", "spline"),
    ("APP_WORDLE", "layout-grid"), ("APP_TERMINAL", "terminal"), ("APP_TODO", "list-checks"),
    ("APP_CLOCK", "clock"), ("APP_CALENDAR", "calendar"), ("APP_MINES", "bomb"),
    ("APP_2048", "grid-2x2"), ("APP_BREAKOUT", "gamepad-2"), ("APP_MONITOR", "activity"),
    ("APP_SETTINGS", "settings"), ("APP_BROWSER", "globe"), ("APP_PLAYER", "circle-play"),
    ("APP_WRITER", "notebook-pen"), ("APP_SHEET", "table"), ("APP_TRASH", "trash-2"),
    ("APP_NATIVE", "cpu"), ("APP_HELLO", "sparkles"), ("APP_ABOUT", "info"),
    ("APP_HELP", "circle-help"), ("POWER", "power"), ("MOON", "moon"), ("MENU", "ellipsis"),
    ("WINDOW", "app-window"), ("LOGO", "circle-dot"), ("MAXIMIZE", "square"),
]


def render(font, char, size):
    big = size * SS
    canvas = Image.new("L", (big * 2, big * 2), 0)
    ImageDraw.Draw(canvas).text((big // 2, big // 2), char, fill=255, font=font)
    box = canvas.getbbox()
    out = Image.new("L", (big, big), 0)
    if box:
        glyph = canvas.crop(box)
        out.paste(glyph, ((big - glyph.width) // 2, (big - glyph.height) // 2))
    return out.resize((size, size), Image.BOX)


def packed(image):
    pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
    levels = [int(round((v / 255.0) ** GAMMA * 15)) for v in pixels]
    data = [(levels[i] << 4) | levels[i + 1] for i in range(0, len(levels), 2)]
    return ",".join(f"0x{b:02x}" for b in data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("package", type=pathlib.Path, help="extracted lucide-static package folder")
    parser.add_argument("--sheet", type=pathlib.Path, help="also write a PNG contact sheet")
    args = parser.parse_args()
    codepoints = json.loads((args.package / "font/codepoints.json").read_text())
    version = json.loads((args.package / "package.json").read_text())["version"]
    ttf = str(args.package / "font/lucide.ttf")
    small_font = ImageFont.truetype(ttf, ICON_SIZE * SS)
    small = [render(small_font, chr(codepoints[name]), ICON_SIZE) for _, name in ICONS]

    lines = [
        "/* Generated by tools/gen_icons.py from Lucide " + version + " (ISC license,",
        " * third_party/lucide/LICENSE). Do not edit. 4 bits of coverage per pixel. */",
        "#ifndef ICONS_H", "#define ICONS_H", "#include <stdint.h>", "",
        f"#define ICON_SIZE {ICON_SIZE}", "", "enum {",
    ]
    lines += [f"    ICON_{name}," for name, _ in ICONS]
    lines += ["    ICON_COUNT", "};", "", "/* Pixel data is compiled once, by gfx.c. */", "#ifdef ICONS_DEFINE_DATA",
              f"static const uint8_t icon_bits[ICON_COUNT][{ICON_SIZE * ICON_SIZE // 2}] = {{"]
    for (name, lucide), image in zip(ICONS, small):
        lines.append(f"    /* {name} ({lucide}) */ {{{packed(image)}}},")
    lines += ["};", "#endif", "", "#endif", ""]
    (ROOT / "src/icons.h").write_text("\n".join(lines))

    if args.sheet:
        cols, cell = 12, ICON_SIZE + 6
        rows = (len(small) + cols - 1) // cols
        sheet = Image.new("L", (cols * cell, rows * cell), 30)
        for i, image in enumerate(small):
            sheet.paste(image, ((i % cols) * cell + 3, (i // cols) * cell + 3))
        sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(args.sheet)
    print(f"Wrote {len(ICONS)} icons at {ICON_SIZE} px to src/icons.h")


if __name__ == "__main__":
    main()
