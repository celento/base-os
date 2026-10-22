# Visual design

BaseOS draws every pixel itself, so the look of the system lives in a small
amount of shared code: palette tokens, two sets of metrics, a control kit and
two icon sets. This guide describes the design those pieces implement and the
rules that apps and shells follow when they draw.

## Two styles

Settings > Appearance switches between two styles. Both use the same token
indices and the same drawing calls; switching only rewrites palette entries
and the metrics struct, so nothing else needs to know which style is active.

| | Classic | Modern |
|---|---|---|
| Window frame | 1px border, 28px accent title bar, 24px menu row inside the window | 38px accent header with rounded corners (muted when inactive), menus in a 28px top bar |
| Window content | `TK_46` | `TK_BASE` |
| Sidebars, status bars | `TK_BASE`, separated by 1px rules | `TK_SUNKEN` |
| Menus, dialogs, launcher | `TK_RAISED` overlay with a hard 2px shadow | `TK_46` overlay with a two-band soft shadow |
| Controls | 26px, square or notched corners | 30px, 6px corners |
| List rows | 24px | 30px |
| Taskbar | 34px bar along the bottom with the Apps button, windows and tray | floating dock with Apps, windows and Show desktop |

The metrics are `ui_theme` (`src/ui/theme.h`): frame sizes, control and row
heights, paddings, scrollbar width, corner radii and the role tokens that
move between indices (`content`, `overlay`, `chrome`, `sidebar`,
`statusbar`, `taskbar`, `shadow2`). Install a style with
`ui_theme_install(style, accent, desktop)`.

## Palette

The 256-entry palette (`src/kernel/gfx.h`) is split into fixed entries and
theme bands:

| Entries | Contents | Changes with the theme |
|---|---|---|
| 0-15 | base colors used by native programs and stored images | no |
| 16-47 | UI tokens (`TK_*`) | yes |
| 48-63 | icon colors and Settings swatches | yes |
| 64-79 | terminal colors (dark in both styles) | yes |
| 80-111 | neutral gray ramp for images | no |
| 112-127 | extra icon colors | yes |
| 128-252 | 5x5x5 color cube | no |

Images and native programs only use the fixed entries, so a style or accent
change never recolors a picture. The accent ramp (`TK_ACCENT_50` to
`TK_ACCENT_700` and `TK_ON_ACCENT`) comes from six presets per style.

## Tokens

Draw with tokens, never with raw palette indices or RGB:

- Surfaces: `TK_RAISED` (fields, buttons), `TK_BASE`, `TK_SUNKEN`, `TK_HOVER`,
  plus the `ui_theme` role fields above.
- Lines: `TK_LINE_SUBTLE` (separators), `TK_LINE` (control borders),
  `TK_LINE_STRONG` (field borders, disabled text).
- Text: `TK_TEXT`, `TK_TEXT2` (labels, metadata), `TK_ICON` (glyphs).
- Accent: `TK_ACCENT_50` (hover, current line), `TK_ACCENT_100` (unfocused
  selection), `TK_ACCENT` (selection, primary button, focus), `TK_ACCENT_700`
  (accent text), `TK_ON_ACCENT`.
- Status: red, amber and green, each with 50, base and 700 steps; `TK_FIND`
  for search hits.

## Control kit

`src/ui/kit.h` draws every control from tokens and `ui_theme`, so one call
gives the right result in both styles. Controls only draw; the caller keeps
the rectangles for hit testing, and a shared layout function computes them
for both drawing and clicks.

- Buttons: `kit_button` (default, primary, danger, flat), `kit_icon_button`.
- Text input: `kit_field`, `kit_search`, `kit_dropdown`, `kit_caret`.
- Choices: `kit_check`, `kit_radio`, `kit_toggle`, `kit_check_row`,
  `kit_segmented`, `kit_tabs`, `kit_slider`.
- Lists: `kit_row` returns the text ink for a row state, `kit_list_header`,
  `kit_vscroll` and `kit_hscroll`.
- Strips: `kit_toolbar`, `kit_statusbar`, separators.
- Feedback: `kit_banner`, `kit_progress`, `kit_tooltip`, `kit_badge`,
  `kit_kbd` for shortcut keys.
- Menus: `kit_menu_frame`, `kit_menu_item`, `kit_menu_sep`.
- Text: `kit_text` cuts with an ellipsis, `kit_text_center`, `kit_text_right`,
  `kit_ty` centers the 17px UI font in a row.

State bits are `KIT_HOVER`, `KIT_PRESSED`, `KIT_FOCUS`, `KIT_DISABLED`,
`KIT_ON`, `KIT_ERROR` and `KIT_UNFOCUSED`. The desktop's `hot(x, y, w, h)`
records a hover rectangle while drawing and returns the state for it; pointer
motion redraws only when the rectangle under the pointer changes.

`ui/toolbar.h` keeps the older `tool_button` API for app toolbars and draws
it through the kit.

## Icons

Two sets, both generated from `assets/ui-icons` by `tools/gen_ui_icons.py`:
16px glyphs (`UI_ICON_FOLDER`, `UI_ICON_SEARCH`, `UI_ICON_BACK` and so on)
drawn in one ink with antialiasing against a paper color, and 32px app-style
icons (`UI_ICON_A_FILES`, `UI_ICON_A_WARN`, ...) with their own colors. Each
style has its own drawing of every icon.

## Layout patterns

- Windows: content starts at `TITLE_H` below the window top; status bars are
  24px (Classic) or 28px (Modern) at the bottom.
- Toolbars hold back, forward and up, then a path bar, then view toggles and
  a search field on the right.
- Sidebars list Places and Devices with 16px glyphs; the current place uses
  `TK_ACCENT_100`.
- File lists have a details view (Name, Size, Kind, Modified with a sort
  arrow; narrow lists drop Kind, then Size, then Modified) and an icon grid;
  dates read "Today 14:30", "Yesterday" or "18 Oct 2026". Kind is brief
  ("Folder", "Text", "C source", "Image"); Properties gives it in full.
- The icon grid and the launcher draw a last row that only partly fits,
  cut at the bottom of the grid, so the view shows there is more to scroll.
- Unsaved documents show a dot after the title, also on their taskbar or
  dock button.
- Dialogs: Open and Save As share the Places sidebar, path bar and detail
  list. Alerts have a 32px icon, a bold question and one line of detail, with
  the buttons on the content (Classic) or on a footer band (Modern).
- Terminal is dark in both styles, with colored prompts, errors and folders.

## Pocket

The Pocket shell (`make pocket`) uses the same tokens and kit in a portrait
frame: a status bar, a header with back, title, subtitle and a more button,
and a bottom bar with Back, Home and Apps. Home has a search field, the app
grid and recent files. App actions live in a menu sheet that slides up from
the bottom bar, and Apps lists open apps as cards. Pocket reads and writes
the same `prefs/style` and `prefs/accent` files as the desktop.
