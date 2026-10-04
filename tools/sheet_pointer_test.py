"""Normal pointer, scrolling, budget seeding and remaining discard paths."""
import argparse
import json
import pathlib

from sheet_input_test import SHEET, SheetSession, SheetCheck


def run(build):
    with SheetSession(build, 'sheet-pointer') as session:
        print(session.directory, flush=True); session.boot(); check = SheetCheck(session, build)
        session.launch('budget.bsh'); check.expect(157, '=SUM(B2:B5)', 3, 1900000)
        check.expect(158, '=SUM(C2:C5)', 3, 1836350); check.expect(159, '=B7-C7', 3, 63650)
        win = check.owner(); x, y = win['x'] + 1, win['y'] + 33
        check.move(x + 94, y + 128)
        session.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
        session.wait(lambda: check.state()['dragging'] == 1 and check.selected()['anchor'] == 26, 'cell drag began at A2')
        check.move(x + 302, y + 176)
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
        check.toolbar(win['w'] - 25, win['h'] - 34 - 35)
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
