# BaseOS overnight spec

Devs build this. Sizes in px. Screen 1280×720, 32bpp VBE LFB (existing). Backbuffer stays 8-bit indexed via `pal32[]`. Do not add a video mode.

Existing chrome constants (keep):
```
MENUBAR_H 32   TITLE_H 28   INFO_H 22   SB 20
ROW_H 24       ICON_S 32    CLOSE_S 13  MENU_ROW 24
BTN_H 26       CHAR_H 16    (Chicago UI, font.h UI_FONT_H)
EDIT_FONT 8×13, advance 9   (font.h — use this for Terminal; not a new face)
```
Menus, close box, striped titles, 1-bit window chrome: unchanged. Title plaque is always white + black text. Close box always white, 13×13, at (wx+6, wy+(TITLE_H-13)/2).

---

## 0. Palette (16 colors)

Keep `COLOR_BLACK=0`, `COLOR_WHITE=15`. Fill `pal32[0..15]` once in `pal_init`. Indices 16–255 unused (black).

| i | hex | name | role |
|---|-----|------|------|
| 0 | #000000 | Black | chrome, text, Paint, Snake playfield |
| 1 | #404040 | DkGray | Paint |
| 2 | #808080 | Gray | Paint, dim, Wordle miss |
| 3 | #C0C0C0 | LtGray | Paint, inactive title fill |
| 4 | #C41E3A | Red | Paint |
| 5 | #E07020 | Orange | Paint |
| 6 | #E0C040 | Yellow | Paint, Wordle present |
| 7 | #2E8B3A | Green | Paint, Wordle exact |
| 8 | #2090A8 | Cyan | Paint |
| 9 | #2040A8 | Blue | Paint |
| 10 | #9030A0 | Magenta | Paint |
| 11 | #8B5A2B | Brown | Paint |
| 12 | #3A5A40 | Pine | desktop (theme) |
| 13 | #203060 | Navy | desktop / title (theme) |
| 14 | #5A3040 | Wine | desktop (theme) |
| 15 | #FFFFFF | White | chrome, Paint |

Paint well uses 12 swatches: 0,1,2,15,4,5,6,7,8,9,10,11 (black, dkgray, gray, white, then the 8 hues + brown). Desktop/title pull from 0–15. Menus stay black/white regardless of theme.

---

## 1. Window manager + taskbar  (build first)

Replace single `state` / `STATE_*` with a window list. One `STATE_*` kind per window. Multiple windows open at once.

```
#define MAX_WIN 8
#define TASKBAR_H 30

enum WinKind {
  WK_FILES, WK_EDIT, WK_CALC, WK_ABOUT, WK_HELP, WK_HELLO,
  WK_SETTINGS, WK_PAINT, WK_VIEW, WK_SNAKE, WK_WORDLE,
  WK_TERM, WK_TODO
};

struct Win {
  int kind, x, y, w, h, open;
  int z;          /* 0 = back, higher = front */
};
```

**Open:** allocating a slot sets `open=1`, assigns next z, copies default size/pos from the table below. Opening the same kind again focuses the existing window (do not spawn a second Calculator, etc.). Files is the exception: Disk and Trash may each have one window (two WK_FILES with different cwd). If that is painful, one Files window that retargets cwd is acceptable.

**Focus:** click any pixel of a window, or its taskbar slot → that window gets max z. Front title gets stripes (existing `draw_title_stripes`) on the theme title color. Others: plain title fill `COLOR_LTGRAY` (3), no stripes, close box still drawn.

**Drag:** press on title bar but not the close box. If mouse moves > 3px before release, drag: `x += dx; y += dy` each mouse packet until button up. Clamp:
- `y >= MENUBAR_H`
- `y + TITLE_H <= fb_h - TASKBAR_H`  (title never under the bar)
- `x + w >= 40`, `x <= fb_w - 40`

A press-release on the title plaque with move < 3px is a **title click** (Files uses this to go up; other apps ignore).

**Close box:** closes that window only (`open=0`), drops it from the taskbar, focuses next-highest z. Empty list → desktop only. ESC = close front window.

**Draw order:** desktop + icons, then windows by z ascending, then menu bar (always on top of windows), then taskbar, then pulldown, then cursor. Menu bar stays y=0..31, full width, black/white.

**Default sizes / positions** (y is below the menu bar):

| kind | w×h | default x,y | chrome flags |
|------|-----|-------------|--------------|
| Files | 720×480 | 80, 60 | WIN_INFO\|WIN_SCROLL |
| Edit | 960×520 | 20, 44 | WIN_INFO\|WIN_SCROLL |
| Calc | 280×380 | centered | 0 (no info, no scroll, no grow) |
| About | 520×280 | centered | 0 |
| Help | 700×280 | centered | 0 |
| Hello | 480×180 | centered | 0 |
| Settings | 560×300 | centered | 0 |
| Paint | 720×460 | 80, 48 | 0 |
| Viewer | image+chrome, clamp 200..800 × 120..520 | centered | 0 |
| Snake | 520×400 | 200, 80 | WIN_INFO |
| Wordle | 440×520 | 220, 56 | 0 |
| Term | 640×400 | 120, 80 | 0 |
| Todo | 420×380 | 180, 90 | 0 |

Centered = `((fb_w-w)/2, MENUBAR_H + (fb_h-MENUBAR_H-TASKBAR_H-h)/2)`. Max height `fb_h - MENUBAR_H - TASKBAR_H - 8`.

### Taskbar

- Strip at `y = fb_h - 30`, `h = 30`, full 1280 wide.
- Fill: white, 1px black line on top. Optional: fill = theme title color if that color's luminance > 128; else white. Text/icons always black on the fill, or white-on-dark if fill luminance < 128.
- **Running apps only.** No Start button. No clock here (clock stays on the menu bar).
- Left-to-right, creation order, 8px left inset. Each slot: 16×16 mini icon, 6px gap, name (Chicago 16), 16px trailing pad. Slot hit box is that whole run.
- Front app: invert the slot (black rect, white glyph+text).
- Click slot → focus that window.
- Does **not** cover Trash. Reposition Trash:

```
icon_x[DISK]  = fb_w - ICON_S - 24           /* 1224 */
icon_y[DISK]  = MENUBAR_H + 16               /* 48 */
icon_x[TRASH] = fb_w - ICON_S - 24           /* 1224 */
icon_y[TRASH] = fb_h - TASKBAR_H - ICON_S - CHAR_H - 12
              /* 720-30-32-16-12 = 630 */
```

Trash label sits just above the taskbar (~8–12px gap). Disk unchanged.

---

## 2. Themes (8) + Settings

Each theme sets **desktop fill/pattern** and **front-window title-bar color**. Menus, close box, title plaque, window body stay black/white.

| id | name | desktop | title |
|----|------|---------|-------|
| 0 | Classic | 50% dither (current `draw_dither`) | white + black stripes (current) |
| 1 | Graphite | solid 1 DkGray | 2 Gray |
| 2 | Navy | solid 13 Navy | 9 Blue |
| 3 | Pine | solid 12 Pine | 7 Green |
| 4 | Wine | solid 14 Wine | 4 Red |
| 5 | Paper | solid 3 LtGray | 0 Black (white stripes) |
| 6 | Cyan | solid 8 Cyan | 13 Navy |
| 7 | Magenta | solid 10 Magenta | 11 Brown |

Front title: fill with title color, then `draw_title_stripes` in black if title luminance > 128, else white. Inactive titles: LtGray fill, no stripes.

**Persist:** hidden dir `/prefs` (same hide rule as `/trash` — skip in volume listing). File `/prefs/theme` = one ASCII byte `'0'..'7'`. Read at `fs_init` after seed; default 0 if missing. Clicking a swatch writes the file and applies immediately.

**Settings window** (was Control Panel). BaseOS menu:

```
About BaseOS
Settings
Help
```

Window 560×300, title "Settings". Body:

- Label "Desktop theme" at (wx+16, wy+TITLE_H+12).
- 8 swatches, 2 rows × 4. Swatch 56×40. Gap 12. Origin (wx+24, wy+TITLE_H+40).
  - `col = i % 4`, `row = i / 4`
  - `sx = ox + col*(56+12)`, `sy = oy + row*(40+28)`  (28 = 12 gap + 16 name)
- Each swatch: fill = desktop color (Classic: dither the 56×40), 8px title-color strip along the top of the swatch, 1px black outer border, name centered under it (Chicago).
- Current theme: extra 1px black rect inset 2px (the "1px inset mark").
- Click swatch → set theme, write `/prefs/theme`, dirty.

Keep the old four status lines out. This window is the theme picker.

---

## 3. Nested folders + seed

FS already has `fs_parent`, `fs_mkdir`, `fs_path`. Disk window:

- Double-click a folder → `fm_cwd = id` (already).
- Info bar shows **path**, not item count. Use `fs_path` into the info string, e.g. `/`, `/Documents`, `/docs`. Clip to window width.
- When `fm_cwd != root`: first list row is `".."` (mini folder icon). Click or double-click → `fm_go_up()`. Backspace still goes up.
- Title-click (see §1) also `fm_go_up()` when not at root. Title text stays the folder name (or "BaseOS" at root) — existing `files_title`.
- **No UP / NEW / OPEN buttons in the window.** File menu keeps New / New Folder / Open / Close / Save as now.

**Seed at `fs_init`** (keep existing `docs/`, `readme.txt`, `Hello`, `trash/`):

```
mkdir  Documents
mkdir  Pictures
mkdir  prefs          (hidden like trash)
create_app Calculator, Paint, Snake, Wordle, Terminal, Todo, Image Viewer
```

All apps are `fs_create_app` nodes in the volume root, **not** `.txt`. Double-click dispatches by name (same pattern as Calculator today).

Bump if tight: `FS_MAX_NODES` 32 → 64, `FS_MAX_SIZE` 512 → 16384 (needed for Paint). `FsNode` at `0x30000` — 64×(~24+16+16384) still well under the 0x90000 stack.

Volume listing hides `trash` and `prefs`.

---

## 4. Calculator

Already built. Do not change layout.

- 280×380, flags 0 (no scroll, no grow, no info).
- Display: (wx+16, wy+TITLE_H+10), size (248×36), white, 1px black inset, digits right-aligned 8px from right.
- Keys 56×50, gap 8, origin under the display +10. Layout:

```
C
7 8 9 /
4 5 6 *
1 2 3 -
0 . = +
```

C is row 0 col 0 only (no ghost keys). App icon already in the volume.

---

## 5. Boot splash + About / Kilroy

**Splash** (before desktop, after `pal_init` + mouse init):

- Fill white. Hold **2.5 s** (count ~150 frames of VGA 0x3DA, or a PIT/busy loop — whatever is already used for timing; do not wait on a key).
- Centered "BaseOS" in Chicago, at (cx, cy-36) where cx centers the string, cy = 360.
- Horizontal black line full-ish width: y = 360, x = 480..800 (320px), 2px thick.
- Kilroy peeking over that line, centered, 1-bit, ~48×28. Then desktop.

**Kilroy** (copy this 48×24 1-bit; `#` = black, `.` = skip / white):

```
.................##.................##
................#..#...............#..#
................#..#...............#..#
................#..#...............#..#
...........#############################
...........#...........................#
............#........#####.............#
.............#......#.....#...........#
..............#....#..#.#..#.........#
...............#...#..#.#..#........#
................#...##...##........#
.................#...............#
..................###############
........................##
.......................#..#
........................##
```

Two round eyes, a blob nose hanging over the wall. Keep it crude. Same bitmap in splash and About.

**About BaseOS** 520×280. Close box. Body, top to bottom, 12px gaps, all centered:

1. Kilroy (wall line at about wy+TITLE_H+56)
2. "BaseOS"
3. "1280x720"
4. "Version 0.6"
5. "(C) 2026 CCG"

---

## 6. Paint + Image Viewer

### Paint  720×460  title "Paint"

```
+-- close -- stripes -- Paint ---------------------+
| [P][F][L][R][E][T][X]   tools 28×28, 4px gap     |  tool strip TOP, 36px
|--------------------------------------------------|
| canvas                                           |
| (rest of body, 1px black inset)                  |
|                                                  |
|--------------------------------------------------|
| [12 color wells 22×18, 4px gap]                  |  well BOTTOM, 28px
+--------------------------------------------------+
```

Tools left-to-right: pencil, fill, line, rect, ellipse, text, eraser. 28×28 cells, 1px black border. Selected tool: 1px inset mark (same language as theme swatches). Icons: 1-bit doodles (pencil = diagonal, fill = bucket, line = `\`, rect = rectangle, ellipse = oval, text = "A", eraser = small rect).

**Canvas** = body minus tool strip minus well. Pencil: plot COLOR at mouse (3×3 for visibility). Eraser: plot white 5×5. Line/rect/ellipse: press-drag-release, rubber-band in XOR or redraw. Fill: 4-connected flood, cap at canvas pixels. Text: click, type until Enter/click; Chicago 16, current color.

Current color = last clicked well. Default black. Wells: 12 swatches as in §0, 22×18, origin (wx+12, wy+h-24). Selected well: 1px inset mark.

**File→Save:** write Pictures. Name `untitled.pbm`; if taken, `fs_unique_file` (`untitled 2.pbm`, …). Format (not ASCII PBM — custom, reloadable):

```
offset  size  value
0       4     'B' 'O' 'S' '1'
4       2     width  le16   (canvas w)
6       2     height le16
8       w*h   palette indices, row-major
```

If `w*h+8 > FS_MAX_SIZE`, refuse (beep/ignore). Canvas is the window canvas at 1:1; keep Paint window at 720×460 so canvas ≈ 718×368 ≈ 264k — **too big**.

**Lock canvas to 160×100 logical pixels**, drawn 3× into a 480×300 area centered in the canvas pane. Save 160×100 = 16000+8 bytes. `FS_MAX_SIZE` = 16384. File→New clears to white.

No layers. One buffer `uint8_t paint_pix[160*100]`.

### Image Viewer  title "Image Viewer"

Double-click in Pictures (or any file whose first 4 bytes are `BOS1`) → open Viewer, decode into a view buffer, window size = image×3 (same 3× scale) + 8px pad + TITLE_H, clamped to the table in §1. Body is the image, nothing else. Close box only.

If the file is **not** `BOS1`: still open a 320×160 window, fill body LtGray, centered "Not a picture".

---

## 7. Snake, Wordle

### Snake  520×400  title "Snake"  WIN_INFO

- Info bar: `Score: N` (N starts 0, +1 per food).
- Playfield: rest of body, **black**. Cell 16×16. Grid 30×20 = 480×320, centered in the body (fits 520-2 × 400-TITLE-INFO-8).
- Snake: white 1-bit blocks, or theme title color if theme ≠ Classic. Food: white 2px inset square. Head = full cell.
- Arrows change direction (ignore 180° reverse). Tick every ~8 frames. Eat → grow 1. Wall or self → freeze, info `Score: N  GAME OVER`.
- File→New (label **New Game** when Snake is front) resets.
- One window. Keys go to Snake only when it is front.

### Wordle  440×520  title "Wordle"

- 6 rows × 5 cells. Cell 44×44, gap 6. Grid origin centered, y = wy+TITLE_H+16.
- Empty cell: white, 1px black. After submit: fill Green(7) exact, Yellow(6) present, Gray(2) miss; black letter.
- On-screen keyboard below the grid, 3 rows QWERTY, keys 28×28, gap 4. Click a letter = type it. Enter submits, Backspace deletes. Physical keyboard also works.
- Random 5-letter word on open and on File→New Game. Hardcode ≥30 words in a `const char *words[]`. Pick `words[frame_count % N]` or any cheap LCG.
- Win: all green. Lose after 6: show the word in the info/title or a one-line caption under the grid.

File menu when Snake/Wordle is front: item 0 label = `"New Game"` (other File items dim except Close).

---

## 8. Terminal, Todo

### Terminal  640×400  title "Terminal"

- Body: black, editor mono (`EDIT_FONT` 8×13, advance 9), green (7) or white glyphs. 8px pad.
- Prompt: `BaseOS:<folder>> ` with a space after `>`.
  - root → `BaseOS:> `
  - Pictures → `BaseOS:Pictures> `
  - nested → `BaseOS:Documents:work> `  (colon-separated names, no leading slash)
- Commands (tokenize on space; extra args ignored unless noted):

| cmd | action |
|-----|--------|
| `ls` | names in cwd, dirs get `:` suffix |
| `cd name` | enter child dir; `cd ..` up; fail = `?` |
| `cat name` | dump file bytes; dirs/`?` if missing |
| `mkdir name` | `fs_mkdir` |
| `rm name` | `fs_delete` (not `.` / `..`) |
| `clear` | wipe scrollback |
| `help` | print the command list |
| `echo ...` | print rest of line |
| unknown | `?` |

- Enter runs. Backspace edits. 80×22 chars roughly. Scrollback: 64 lines, ring. No cursor blink required; a 1px block caret is enough.
- cwd is Terminal's own, independent of the Files window.

### Todo  420×380  title "Todo"

- Rows from y = wy+TITLE_H+8: circle 12×12 (1px black, empty = open, filled black disc = done) at x+12, label at x+32, row height 24. Click the circle (or the row) to toggle.
- Bottom 36px: 1px black rule, then a text field "new item" (click to focus, type, Enter appends, max 24 chars / `FS_NAME_LEN`).
- No dates, no times, no priorities.
- Persist `/prefs/todo`: one line per item, `'x'` or `' '` then the text, `\n` separated. Load on open, write on every toggle/add. Cap 12 items.

---

## Menus (delta)

```
BaseOS:  About BaseOS | Settings | Help
File:    New | New Folder | Open | Close | Save
         (label New → "New Game" when Snake or Wordle is front)
Edit:    Cut | Copy | Paste     (unchanged enable rules)
Special: Empty Trash | - | Shutdown
```

File→Close = close-box on the front window. File→Save: Edit saves text; Paint writes Pictures; others dim. File→New: Edit new buffer; Paint clear; Snake/Wordle new game; Todo focuses the new-item field; Files new untitled file (existing).

---

## Build order

1. Window list + title-bar drag + taskbar (Trash y moved). Still 1-bit is fine.
2. 16-color `pal_init` + 8 themes + Settings swatches + `/prefs/theme`.
3. Nested nav (`..`, path in info, title-click up) + seed Documents/Pictures + app nodes.
4. Calculator — leave it unless it broke in (1).
5. Kilroy About + 2.5s boot splash.
6. Paint (160×100, tools, well, Save BOS1) + Image Viewer.
7. Snake, Wordle.
8. Terminal, Todo.

Wireframes in this folder: `wf-desktop-taskbar.png`, `wf-settings-themes.png`, `wf-paint.png`, `wf-boot.png`, `wf-multitask.png`.
