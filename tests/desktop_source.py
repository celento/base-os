"""Shared extraction helpers for harnesses built from src/shell/desktop/desktop.c."""
import re
from pathlib import Path

# Window chrome geometry follows the active style (src/ui/theme.c); these
# macros read ui_theme, so harnesses that use them include theme_metrics_host.h.
CHROME_MACROS = ('MODERN', 'MENUBAR_H', 'TITLE_BAR', 'WMENU_H', 'TITLE_H')


def define(source, name):
    match = re.search(r'^#define ' + name + r'\b[^\n]*\n', source, re.M)
    if not match:
        raise ValueError('Missing production macro: ' + name)
    return match.group()


def defines(source, names):
    return ''.join(define(source, name) for name in names)


def chrome_defines(source):
    return defines(source, CHROME_MACROS)


def block(source, start, end, include_end=True):
    """Source text from the first start marker through the next end marker."""
    first = source.index(start)
    last = source.index(end, first + len(start))
    return source[first:last + (len(end) if include_end else 0)]


def desktop_input_defs(source):
    """Chrome geometry, window behavior options and double-click timing that
    the production pointer adapter reads; desktop_input_host.c includes these
    after theme_metrics_host.h."""
    platform = (Path(__file__).resolve().parents[1] / 'src/kernel/platform.h').read_text()
    code = define(platform, 'TIMER_HZ')
    code += chrome_defines(source) + defines(source, ('TASKBAR_H', 'TASKBAR_Y', 'MENU_BAR'))
    code += block(source, '/* Window behavior:', '#define SNAP_DISTANCE_MAX 32\n')
    code += block(source, '/* Double-click speed', 'static const char *session_status', include_end=False)
    return code


def desktop_input_kernel(source, function, names=()):
    """Gesture helpers, any extra named functions and the ordered adapter."""
    code = ''.join(function(source, name) for name in (
        'input_gesture_ticks', 'double_click', 'win_resize_tick', 'win_snap_edges', 'saver_start',
        'doc_client') + tuple(names))
    return code + block(source, '/* Desktop-only compatibility adapter',
                        '/* ---------- Shell contract (kernel/shell.h)', include_end=False)
