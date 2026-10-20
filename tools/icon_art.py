"""The BaseOS application icon set, as SVG.

Each icon is an object lit from the top left with a soft gloss and a small
drop shadow. The drawings live
in this file so the whole set shares one palette and one light source.
Running the module writes assets/icons/<name>.svg; tools/gen_app_icons.py
rasterizes them into src/ui/app_icons.h.
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/icons"

# Order defines the AI_* enum in src/ui/app_icons.h.
ICONS = []


def icon(name):
    def wrap(fn):
        ICONS.append((name, fn))
        return fn
    return wrap


def grad(id_, stops, x1=0, y1=0, x2=0, y2=1):
    body = "".join(f'<stop offset="{o}" stop-color="{c}"{"" if a is None else f" stop-opacity={chr(34)}{a}{chr(34)}"}/>'
                   for o, c, a in [(s[0], s[1], s[2] if len(s) > 2 else None) for s in stops])
    return f'<linearGradient id="{id_}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">{body}</linearGradient>'


def rgrad(id_, stops, cx=0.5, cy=0.5, r=0.5):
    body = "".join(f'<stop offset="{o}" stop-color="{c}"{"" if a is None else f" stop-opacity={chr(34)}{a}{chr(34)}"}/>'
                   for o, c, a in [(s[0], s[1], s[2] if len(s) > 2 else None) for s in stops])
    return f'<radialGradient id="{id_}" cx="{cx}" cy="{cy}" r="{r}">{body}</radialGradient>'


SHADOW = '<ellipse cx="32" cy="57" rx="20" ry="4" fill="#000" opacity="0.22"/>'
GLOSS = grad("gloss", [(0, "#fff", 0.55), (1, "#fff", 0.0)])


def svg(defs, body, shadow=True):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
            f"<defs>{GLOSS}{defs}</defs>{SHADOW if shadow else ''}{body}</svg>")


def badge(light, dark, edge, extra_defs="", x=8, y=6, w=48, h=48, rx=12, gid="b"):
    """A glossy rounded square: the base for app-style icons."""
    defs = grad(gid, [(0, light), (1, dark)]) + extra_defs
    body = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="url(#{gid})" stroke="{edge}" stroke-width="1.5"/>'
            f'<path d="M{x + 2} {y + rx} a{rx - 2} {rx - 2} 0 0 1 {rx - 2} -{rx - 2} h{w - 2 * rx} a{rx - 2} {rx - 2} 0 0 1 {rx - 2} {rx - 2} v{h / 2 - rx - 1} h-{w - 4} z" fill="url(#gloss)"/>')
    return defs, body


@icon("files")
def files():
    defs = (grad("fb", [(0, "#5aa8f0"), (1, "#1f6fcf")]) + grad("ff", [(0, "#a9d4ff"), (1, "#3d8fe6")]))
    body = ('<path d="M9 14 h16 l5 5 h25 a4 4 0 0 1 4 4 v27 a4 4 0 0 1 -4 4 h-46 a4 4 0 0 1 -4 -4 v-32 a4 4 0 0 1 4 -4 z" fill="url(#fb)" stroke="#1a5aa8" stroke-width="1.5"/>'
            '<path d="M5 27 h54 a3 3 0 0 1 3 3 l-3 19 a4 4 0 0 1 -4 4 h-46 a4 4 0 0 1 -4 -4 z" fill="url(#ff)" stroke="#1d63b8" stroke-width="1.5"/>'
            '<path d="M7 29 h50 l-1.5 10 h-47 z" fill="url(#gloss)"/>')
    return defs, body


@icon("editor")
def editor():
    defs = grad("pg", [(0, "#ffffff"), (1, "#e6e9ef")]) + grad("pen", [(0, "#ffd54a"), (1, "#e0a000")])
    lines = "".join(f'<rect x="18" y="{y}" width="{w}" height="2" rx="1" fill="#9fb3cc"/>' for y, w in ((20, 24), (27, 28), (34, 22), (41, 26)))
    body = ('<path d="M13 7 h30 l9 9 v41 a2 2 0 0 1 -2 2 h-37 a2 2 0 0 1 -2 -2 v-48 a2 2 0 0 1 2 -2 z" fill="url(#pg)" stroke="#8a95a8" stroke-width="1.5"/>'
            '<path d="M43 7 v7 a2 2 0 0 0 2 2 h7 z" fill="#c9d1dd" stroke="#8a95a8" stroke-width="1.2"/>'
            '<rect x="14" y="8" width="6" height="49" fill="#ffd9d9" opacity="0.8"/>' + lines +
            '<g transform="rotate(45 44 44)"><rect x="40" y="26" width="8" height="26" rx="1" fill="url(#pen)" stroke="#9c6f00" stroke-width="1"/>'
            '<path d="M40 52 l4 7 l4 -7 z" fill="#f0d9b5" stroke="#9c6f00" stroke-width="1"/><path d="M43 57 l1 2 l1 -2 z" fill="#333"/>'
            '<rect x="40" y="26" width="8" height="4" fill="#ff8aa0"/></g>')
    return defs, body


@icon("calc")
def calc():
    defs, body = badge("#6b7382", "#2f3542", "#1e232c", gid="cb")
    defs += grad("lcd", [(0, "#dff7c8"), (1, "#a9d98a")])
    body += '<rect x="14" y="12" width="36" height="12" rx="2" fill="url(#lcd)" stroke="#223" stroke-width="1"/>'
    body += '<text x="47" y="22" font-family="Arial" font-size="9" font-weight="bold" text-anchor="end" fill="#243b1a">1024</text>'
    colors = ["#f3f5f8", "#f3f5f8", "#f3f5f8", "#ff9f43"] * 3
    i = 0
    for row in range(3):
        for col in range(4):
            x, y = 14 + col * 9.3, 29 + row * 8
            body += f'<rect x="{x:.1f}" y="{y}" width="7.5" height="6" rx="1.5" fill="{colors[i]}" stroke="#1e232c" stroke-width="0.6"/>'
            i += 1
    return defs, body


@icon("paint")
def paint():
    defs = grad("pal", [(0, "#f4e2c4"), (1, "#c9a26a")]) + grad("brush", [(0, "#8d5a2b"), (1, "#5a3615")])
    blobs = [(22, 22, "#e63946"), (34, 16, "#ffb703"), (46, 24, "#2a9d8f"), (18, 36, "#3a86ff"), (30, 32, "#8338ec")]
    body = ('<path d="M32 8 c15 0 25 9 25 20 c0 7 -5 10 -11 10 c-4 0 -6 2 -6 5 c0 4 4 4 4 8 c0 4 -5 7 -12 7 c-15 0 -26 -11 -26 -25 c0 -14 11 -25 26 -25 z" fill="url(#pal)" stroke="#8a6a3a" stroke-width="1.5"/>'
            + "".join(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{c}" stroke="#00000033" stroke-width="1"/>' for x, y, c in blobs)
            + '<path d="M30 8 c10 0 18 4 22 10 c-6 -3 -14 -5 -22 -5 c-8 0 -14 2 -18 5 c4 -6 9 -10 18 -10 z" fill="url(#gloss)"/>'
            '<g transform="rotate(-40 44 44)"><rect x="41" y="30" width="6" height="22" rx="2" fill="url(#brush)"/><rect x="40" y="50" width="8" height="4" fill="#d0d0d0"/><path d="M40 54 h8 l-2 8 h-4 z" fill="#3a86ff"/></g>')
    return defs, body


@icon("viewer")
def viewer():
    defs = grad("sky", [(0, "#6ec6ff"), (1, "#bfe6ff")]) + grad("hill", [(0, "#7ccf5a"), (1, "#3d8f2e")])
    body = ('<rect x="8" y="10" width="48" height="44" rx="3" fill="#ffffff" stroke="#9aa5b5" stroke-width="1.5"/>'
            '<rect x="12" y="14" width="40" height="32" fill="url(#sky)"/>'
            '<circle cx="42" cy="23" r="5" fill="#ffd93d"/>'
            '<path d="M12 46 l10 -14 l8 9 l6 -6 l16 11 v0 h-40 z" fill="url(#hill)"/>'
            '<path d="M12 14 h40 v10 h-40 z" fill="url(#gloss)"/>'
            '<rect x="8" y="10" width="48" height="44" rx="3" fill="none" stroke="#9aa5b5" stroke-width="1.5"/>')
    return defs, body


@icon("snake")
def snake():
    defs, body = badge("#3f8f3a", "#1f5a20", "#153f16", gid="sb")
    defs += grad("sn", [(0, "#b8f08a"), (1, "#5cb846")])
    body += ('<path d="M18 44 c0 -8 8 -8 14 -8 s14 0 14 -8 s-8 -8 -14 -8 h-8" fill="none" stroke="url(#sn)" stroke-width="7" stroke-linecap="round"/>'
             '<path d="M18 44 c0 -8 8 -8 14 -8 s14 0 14 -8 s-8 -8 -14 -8 h-8" fill="none" stroke="#1f5a20" stroke-width="1" stroke-linecap="round" opacity="0.5"/>'
             '<circle cx="22" cy="20" r="5" fill="#8fe06a" stroke="#1f5a20" stroke-width="1"/><circle cx="20.5" cy="19" r="1.2" fill="#111"/>'
             '<path d="M17 20 l-5 -1 l5 2 z" fill="#e63946"/><circle cx="46" cy="42" r="3" fill="#ff5a5a"/>')
    return defs, body


@icon("wordle")
def wordle():
    defs = grad("tg", [(0, "#8fd36a"), (1, "#4f9a3a")]) + grad("ty", [(0, "#ffd95a"), (1, "#d9a400")]) + grad("tk", [(0, "#9aa3b0"), (1, "#5f6873")])
    tiles = [(10, 10, "tg", "B"), (34, 10, "ty", "A"), (10, 34, "tk", "S"), (34, 34, "tg", "E")]
    body = "".join(f'<rect x="{x}" y="{y}" width="20" height="20" rx="3" fill="url(#{g})" stroke="#00000044" stroke-width="1"/>'
                   f'<text x="{x + 10}" y="{y + 15}" font-family="Arial" font-size="13" font-weight="bold" text-anchor="middle" fill="#fff">{t}</text>'
                   for x, y, g, t in tiles)
    body += '<path d="M10 13 a3 3 0 0 1 3 -3 h14 a3 3 0 0 1 3 3 v7 h-20 z" fill="url(#gloss)"/><path d="M34 13 a3 3 0 0 1 3 -3 h14 a3 3 0 0 1 3 3 v7 h-20 z" fill="url(#gloss)"/>'
    return defs, body


@icon("terminal")
def terminal():
    defs, body = badge("#4a505c", "#1b1e25", "#0d0f13", gid="tb")
    body += ('<rect x="12" y="11" width="40" height="38" rx="3" fill="#0b0d12" stroke="#2f3540" stroke-width="1"/>'
             '<text x="16" y="29" font-family="Courier New" font-size="14" font-weight="bold" fill="#7CFC7C">&gt;_</text>'
             '<rect x="12" y="11" width="40" height="12" rx="3" fill="url(#gloss)" opacity="0.5"/>')
    return defs, body


@icon("todo")
def todo():
    defs = grad("board", [(0, "#c9915a"), (1, "#8f5a2b")]) + grad("sheet", [(0, "#ffffff"), (1, "#e8ecf2")])
    rows = "".join(f'<rect x="16" y="{y}" width="8" height="8" rx="2" fill="#f8fafc" stroke="#7a8696" stroke-width="1"/>'
                   f'<path d="M17.5 {y + 4} l2.5 2.5 l4 -5" fill="none" stroke="#2fa84f" stroke-width="2" stroke-linecap="round"/>'
                   f'<rect x="28" y="{y + 2}" width="20" height="3" rx="1.5" fill="#9fb0c4"/>' for y in (22, 33, 44))
    body = ('<rect x="10" y="10" width="44" height="48" rx="5" fill="url(#board)" stroke="#6b3f17" stroke-width="1.5"/>'
            '<rect x="14" y="16" width="36" height="38" rx="2" fill="url(#sheet)" stroke="#9aa5b5" stroke-width="1"/>'
            '<rect x="24" y="6" width="16" height="9" rx="3" fill="#5f6873" stroke="#2f3540" stroke-width="1"/>' + rows)
    return defs, body


@icon("clock")
def clock():
    defs = grad("rim", [(0, "#f2f5fa"), (1, "#9aa6b8")]) + rgrad("face", [(0, "#ffffff"), (1, "#dfe5ee")], 0.4, 0.35, 0.7)
    ticks = "".join(f'<rect x="31" y="12" width="2" height="5" fill="#334" transform="rotate({a} 32 32)"/>' for a in range(0, 360, 30))
    body = ('<circle cx="32" cy="32" r="26" fill="url(#rim)" stroke="#5c6b80" stroke-width="1.5"/>'
            '<circle cx="32" cy="32" r="22" fill="url(#face)" stroke="#8a97ab" stroke-width="1"/>' + ticks +
            '<path d="M32 32 l-7 7" stroke="#1d3557" stroke-width="3" stroke-linecap="round"/>'
            '<path d="M32 32 l0 -14" stroke="#1d3557" stroke-width="3" stroke-linecap="round"/>'
            '<path d="M32 32 l10 -6" stroke="#e63946" stroke-width="1.5" stroke-linecap="round"/>'
            '<circle cx="32" cy="32" r="2.2" fill="#1d3557"/>'
            '<path d="M14 30 a18 18 0 0 1 36 0 z" fill="url(#gloss)" opacity="0.7"/>')
    return defs, body


@icon("calendar")
def calendar():
    defs = grad("cal", [(0, "#ff7b7b"), (1, "#d62828")]) + grad("page", [(0, "#ffffff"), (1, "#eef1f6")])
    body = ('<rect x="9" y="12" width="46" height="46" rx="5" fill="url(#page)" stroke="#8a95a8" stroke-width="1.5"/>'
            '<path d="M9 17 a5 5 0 0 1 5 -5 h36 a5 5 0 0 1 5 5 v10 h-46 z" fill="url(#cal)"/>'
            '<path d="M9 17 a5 5 0 0 1 5 -5 h36 a5 5 0 0 1 5 5 v4 h-46 z" fill="url(#gloss)"/>'
            '<rect x="18" y="7" width="4" height="10" rx="2" fill="#5f6873"/><rect x="42" y="7" width="4" height="10" rx="2" fill="#5f6873"/>'
            '<text x="32" y="51" font-family="Arial" font-size="22" font-weight="bold" text-anchor="middle" fill="#2b3340">16</text>')
    return defs, body


@icon("mines")
def mines():
    defs = grad("tile", [(0, "#e6e9ef"), (1, "#b7c0cc")]) + rgrad("bomb", [(0, "#6e7682"), (1, "#111419")], 0.35, 0.3, 0.7)
    body = ('<rect x="8" y="8" width="48" height="48" rx="4" fill="url(#tile)" stroke="#7d8794" stroke-width="1.5"/>'
            '<circle cx="31" cy="36" r="14" fill="url(#bomb)"/>'
            '<rect x="29" y="18" width="4" height="6" fill="#333"/>'
            '<path d="M31 18 c2 -6 8 -6 10 -10" fill="none" stroke="#8a5a2b" stroke-width="2"/>'
            '<circle cx="42" cy="8.5" r="3" fill="#ffb703"/><circle cx="42" cy="8.5" r="1.4" fill="#fff"/>'
            '<circle cx="26" cy="30" r="3" fill="#fff" opacity="0.5"/>')
    return defs, body


@icon("g2048")
def g2048():
    defs, body = badge("#f7b267", "#e07a1f", "#a3540c", gid="gb")
    body += ('<text x="32" y="40" font-family="Arial" font-size="19" font-weight="bold" text-anchor="middle" fill="#fff" stroke="#9a4a0a" stroke-width="0.6">2048</text>')
    return defs, body


@icon("breakout")
def breakout():
    defs, body = badge("#4c5d7a", "#1f2a3d", "#121a28", gid="bb")
    colors = ["#e63946", "#ffb703", "#2a9d8f", "#3a86ff"]
    for row in range(2):
        for col in range(4):
            body += f'<rect x="{13 + col * 9.6}" y="{12 + row * 7}" width="8.5" height="5.5" rx="1" fill="{colors[(row + col) % 4]}"/>'
    body += '<circle cx="33" cy="36" r="3.5" fill="#fff"/><rect x="24" y="46" width="18" height="4" rx="2" fill="#dfe6f2"/>'
    return defs, body


@icon("monitor")
def monitor():
    defs, body = badge("#3b4250", "#161a22", "#0c0e13", gid="mb")
    body += ('<path d="M13 36 h8 l4 -10 l5 18 l5 -22 l5 14 h11" fill="none" stroke="#3ef28a" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>'
             '<path d="M13 36 h8 l4 -10 l5 18 l5 -22 l5 14 h11" fill="none" stroke="#bfffd9" stroke-width="1" opacity="0.6"/>')
    return defs, body


@icon("settings")
def settings():
    defs = grad("gear", [(0, "#d7dde6"), (1, "#7b8594")])
    teeth = "".join(f'<rect x="29" y="6" width="6" height="10" rx="1.5" fill="url(#gear)" stroke="#4e5868" stroke-width="1" transform="rotate({a} 32 32)"/>' for a in range(0, 360, 45))
    body = (teeth + '<circle cx="32" cy="32" r="19" fill="url(#gear)" stroke="#4e5868" stroke-width="1.5"/>'
            '<circle cx="32" cy="32" r="8" fill="#3a4454" stroke="#232a36" stroke-width="1.5"/>'
            '<circle cx="32" cy="32" r="4" fill="#9fb3cc"/>'
            '<path d="M15 30 a17 17 0 0 1 34 0 z" fill="url(#gloss)" opacity="0.6"/>')
    return defs, body


@icon("browser")
def browser():
    defs = rgrad("globe", [(0, "#9fd8ff"), (0.6, "#2d8fe0"), (1, "#0f4f9c")], 0.35, 0.3, 0.75)
    body = ('<circle cx="32" cy="32" r="25" fill="url(#globe)" stroke="#0b3f7d" stroke-width="1.5"/>'
            '<g fill="none" stroke="#e8f4ff" stroke-width="1.6" opacity="0.9">'
            '<ellipse cx="32" cy="32" rx="11" ry="25"/><ellipse cx="32" cy="32" rx="25" ry="10"/>'
            '<path d="M7 32 h50 M32 7 v50"/></g>'
            '<path d="M9 28 a23 23 0 0 1 46 0 z" fill="url(#gloss)" opacity="0.55"/>')
    return defs, body


@icon("player")
def player():
    defs, body = badge("#b07cf0", "#6a35c2", "#44207f", gid="pb")
    body += ('<circle cx="32" cy="31" r="15" fill="#ffffff" opacity="0.18"/>'
             '<path d="M26 21 l15 10 l-15 10 z" fill="#ffffff" stroke="#3f1d7a" stroke-width="1"/>')
    return defs, body


@icon("writer")
def writer():
    defs = grad("wpg", [(0, "#ffffff"), (1, "#e6e9ef")]) + grad("nib", [(0, "#9fd0ff"), (1, "#1f6fcf")])
    lines = "".join(f'<rect x="17" y="{y}" width="{w}" height="2.2" rx="1.1" fill="#8fa3bc"/>' for y, w in ((16, 20), (23, 30), (30, 26), (37, 30), (44, 18)))
    body = ('<path d="M11 7 h32 l9 9 v41 a2 2 0 0 1 -2 2 h-39 a2 2 0 0 1 -2 -2 v-48 a2 2 0 0 1 2 -2 z" fill="url(#wpg)" stroke="#8a95a8" stroke-width="1.5"/>'
            '<path d="M43 7 v7 a2 2 0 0 0 2 2 h7 z" fill="#c9d1dd" stroke="#8a95a8" stroke-width="1.2"/>' + lines +
            '<g transform="rotate(40 46 42)"><rect x="42" y="18" width="9" height="30" rx="2" fill="#2b3340"/>'
            '<path d="M42 48 h9 l-4.5 12 z" fill="url(#nib)" stroke="#0f4f9c" stroke-width="1"/><circle cx="46.5" cy="52" r="1" fill="#0f4f9c"/>'
            '<rect x="42" y="22" width="9" height="3" fill="#c0c8d4"/></g>')
    return defs, body


@icon("sheet")
def sheet():
    defs = grad("shg", [(0, "#4fc36a"), (1, "#1f8a3a")]) + grad("cells", [(0, "#ffffff"), (1, "#eef3ee")])
    grid = "".join(f'<path d="M15 {y} h34" stroke="#b9c7b9" stroke-width="1"/>' for y in (26, 34, 42, 50))
    grid += "".join(f'<path d="M{x} 18 v38" stroke="#b9c7b9" stroke-width="1"/>' for x in (26, 38))
    body = ('<rect x="9" y="8" width="46" height="50" rx="4" fill="url(#cells)" stroke="#4a7a52" stroke-width="1.5"/>'
            '<path d="M9 12 a4 4 0 0 1 4 -4 h38 a4 4 0 0 1 4 4 v8 h-46 z" fill="url(#shg)"/>'
            '<path d="M9 12 a4 4 0 0 1 4 -4 h38 a4 4 0 0 1 4 4 v4 h-46 z" fill="url(#gloss)"/>'
            '<rect x="9" y="20" width="8" height="38" fill="#dfe8df"/>' + grid +
            '<rect x="27" y="35" width="10" height="7" fill="#4fc36a" opacity="0.6"/>')
    return defs, body


@icon("trash")
def trash():
    defs = grad("bin", [(0, "#dfe4ec"), (0.5, "#aeb7c4"), (1, "#eef1f5")], 0, 0, 1, 0) + grad("lid", [(0, "#e9edf3"), (1, "#9aa5b5")])
    body = ('<path d="M16 20 h32 l-3 36 a3 3 0 0 1 -3 3 h-20 a3 3 0 0 1 -3 -3 z" fill="url(#bin)" stroke="#6b7684" stroke-width="1.5"/>'
            + "".join(f'<path d="M{x} 25 v30" stroke="#8a95a8" stroke-width="1.5" opacity="0.7"/>' for x in (25, 32, 39)) +
            '<rect x="12" y="14" width="40" height="7" rx="3" fill="url(#lid)" stroke="#6b7684" stroke-width="1.5"/>'
            '<rect x="26" y="9" width="12" height="6" rx="2" fill="#aeb7c4" stroke="#6b7684" stroke-width="1.2"/>')
    return defs, body


@icon("native")
def native():
    defs, body = badge("#48525f", "#1d2229", "#0f1217", gid="nb")
    pins = "".join(f'<rect x="{x}" y="9" width="3" height="6" fill="#e0b84a"/><rect x="{x}" y="49" width="3" height="6" fill="#e0b84a"/>' for x in range(20, 44, 7))
    pins += "".join(f'<rect x="9" y="{y}" width="6" height="3" fill="#e0b84a"/><rect x="49" y="{y}" width="6" height="3" fill="#e0b84a"/>' for y in range(20, 44, 7))
    body += pins + '<rect x="17" y="17" width="30" height="30" rx="3" fill="#2f3b4a" stroke="#8aa0bb" stroke-width="1.2"/><rect x="24" y="24" width="16" height="16" rx="2" fill="#4c6480"/>'
    return defs, body


@icon("hello")
def hello():
    defs = grad("star", [(0, "#ffe76a"), (1, "#f5a300")])
    body = ('<path d="M32 8 l5 15 l16 1 l-12.5 10 l4.5 15.5 l-13 -9 l-13 9 l4.5 -15.5 l-12.5 -10 l16 -1 z" fill="url(#star)" stroke="#b86e00" stroke-width="1.5" stroke-linejoin="round"/>'
            '<path d="M32 12 l3.5 11 l11 0.5 l-10 7 h-9 l-10 -7 l11 -0.5 z" fill="url(#gloss)"/>'
            '<circle cx="50" cy="12" r="2.5" fill="#ffd84a"/><circle cx="12" cy="18" r="1.8" fill="#ffd84a"/>')
    return defs, body


def round_badge(light, dark, edge, gid):
    defs = grad(gid, [(0, light), (1, dark)])
    body = (f'<circle cx="32" cy="31" r="24" fill="url(#{gid})" stroke="{edge}" stroke-width="1.5"/>'
            '<path d="M10 28 a22 22 0 0 1 44 0 z" fill="url(#gloss)" opacity="0.7"/>')
    return defs, body


@icon("about")
def about():
    defs, body = round_badge("#6fb3ff", "#1f66c9", "#0f4594", "ab")
    body += '<text x="32" y="41" font-family="Georgia" font-size="28" font-weight="bold" font-style="italic" text-anchor="middle" fill="#fff">i</text>'
    return defs, body


@icon("help")
def help_():
    defs, body = round_badge("#6fb3ff", "#1f66c9", "#0f4594", "hb")
    body += '<text x="32" y="41" font-family="Arial" font-size="26" font-weight="bold" text-anchor="middle" fill="#fff">?</text>'
    return defs, body


@icon("document")
def document():
    defs = grad("dpg", [(0, "#ffffff"), (1, "#e3e7ee")])
    lines = "".join(f'<rect x="19" y="{y}" width="{w}" height="2.2" rx="1.1" fill="#a5b4c8"/>' for y, w in ((24, 22), (31, 26), (38, 20), (45, 24)))
    body = ('<path d="M14 7 h28 l9 9 v41 a2 2 0 0 1 -2 2 h-35 a2 2 0 0 1 -2 -2 v-48 a2 2 0 0 1 2 -2 z" fill="url(#dpg)" stroke="#8a95a8" stroke-width="1.5"/>'
            '<path d="M42 7 v7 a2 2 0 0 0 2 2 h7 z" fill="#c9d1dd" stroke="#8a95a8" stroke-width="1.2"/>' + lines)
    return defs, body


@icon("window")
def window():
    defs, body = badge("#8fb4e8", "#3c6fb8", "#28508a", gid="wb")
    body += ('<rect x="14" y="14" width="36" height="32" rx="3" fill="#f4f7fb" stroke="#28508a" stroke-width="1"/>'
             '<rect x="14" y="14" width="36" height="8" rx="3" fill="#c9d9f0"/>'
             '<circle cx="19" cy="18" r="1.6" fill="#e63946"/><circle cx="24" cy="18" r="1.6" fill="#ffb703"/><circle cx="29" cy="18" r="1.6" fill="#2fa84f"/>')
    return defs, body


def write_all():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in ICONS:
        defs, body = fn()
        (OUT / f"{name}.svg").write_text(svg(defs, body) + "\n")
    return [name for name, _ in ICONS]


if __name__ == "__main__":
    print("\n".join(write_all()))
