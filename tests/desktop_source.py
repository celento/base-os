"""Shared extraction helpers for harnesses built from src/shell/desktop/desktop.c."""
import re

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
