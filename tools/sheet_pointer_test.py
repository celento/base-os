"""Normal pointer, scrolling, budget seeding and remaining discard paths."""
import argparse
import json
import pathlib
import struct

from elf_debug import DebugInfo

from sheet_input_test import SHEET, SheetSession, SheetCheck


def client(session, build, check, win):
    """The Spreadsheet client origin (Classic: below the menu row) and its Frame layout."""
    debug = DebugInfo(pathlib.Path(build) / 'kernel.elf')
    def fields(name, unit):
        address, (size, offsets) = debug.variable(name, unit)
        raw = session.memory(address, size)
        return {k: struct.unpack_from('<i', raw, o)[0] for k, o in offsets.items() if o + 4 <= size}
    size, offsets = debug.structure('UiTheme', 'src/ui/theme.c')
    raw = check.o.read('ui_theme', size)
    theme = {k: struct.unpack_from('<i', raw, o)[0] for k, o in offsets.items() if o + 4 <= size}
    modern = theme['style'] == 1
    title_bar = theme['border'] + theme['title_h'] + (0 if theme['menu_h'] else 1)
    top = win['y'] + (title_bar - 1 if modern else title_bar + theme['menu_h'])
    return win['x'] + 1, top, fields('fr', 'src/apps/sheet.c')


def run(build):
    with SheetSession(build, 'sheet-pointer') as session:
        print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
        session.launch('budget.bsh'); check.expect(157, '=SUM(B2:B5)', 3, 1900000)
        check.expect(158, '=SUM(C2:C5)', 3, 1836350); check.expect(159, '=B7-C7', 3, 63650)
        win = check.owner(); x, y, fr = client(session, build, check, win)
        widths = check.widths()
        def cell(col, row):
            left = fr['row_w'] + sum(widths[:col]) - check.state()['scroll_x']
            return x + left + widths[col] // 2, y + fr['grid_y'] + row * 24 + 12
        check.move(*cell(0, 1))
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
        session.wait(lambda: check.state()['dragging'] == 1 and check.selected()['anchor'] == 26, 'cell drag began at A2')
        check.move(*cell(2, 3))
        session.wait(lambda: check.selected()['caret'] == 80, 'cell range drag reaches C4')
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
        session.wait(lambda: check.state()['dragging'] == 0, 'mouse release ends range drag')
        assert check.selected()['anchor'] == 26
        session.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'down': True, 'button': 'wheel-down'}},
            {'type': 'btn', 'data': {'down': False, 'button': 'wheel-down'}}]})
        session.wait(lambda: check.state()['first_row'] > 0, 'wheel scrolls the Spreadsheet under the pointer')
        assert check.selected()['caret'] == 80 and check.selected()['anchor'] == 26
        session.key('ctrl-home'); session.wait(lambda: check.state()['first_row'] == 0, 'keyboard reveals A1')
        check.click(x + fr['right'] - 10, y + fr['bottom'] + fr['sb'] // 2)
        session.wait(lambda: check.state()['first_col'] > 0, 'horizontal scrollbar explores columns')
        session.key('ctrl-home'); session.wait(lambda: check.state()['first_col'] == 0, 'keyboard reveals column A')
        check.jump('D7')
        picture = session.screenshot('spreadsheet-original-budget.png')
        session.key('ctrl-n'); check.expect(0, b'', 0)
        session.text('Discard new'); session.key('ctrl-n'); check.modal(); check.choose('discard'); check.expect(0, b'', 0)
        session.text('Discard close'); session.key('ctrl-w'); check.modal(); check.choose('discard')
        session.wait(lambda: not any(w['open'] and w['kind'] == SHEET for w in check.o.windows()), 'explicit Discard closes the sheet')
        result = {'passed': True, 'screenshot': str(picture),
                  'checks': ['budget seeding and guest formula totals on boot floppy', 'PS/2 cell click/drag/release',
                             'mouse wheel and horizontal scrollbar preserve selection', 'New/Close explicit Discard']}
        (session.directory / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
