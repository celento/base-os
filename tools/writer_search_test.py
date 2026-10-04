"""Production Writer Find/Replace, menu routing and minimum-width UI checks."""
import argparse
import json
import pathlib
import tempfile

from writer_input_test import WriterSession, WriterCheck, fixture, native


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-search-data-'))
    text = b'alpha ALPHA alpha\nomega'
    style = bytearray(len(text) + 1)
    style[:5] = bytes([1]) * 5; style[6:11] = bytes([2]) * 5; style[12:17] = bytes([4]) * 5
    paragraphs = bytearray(len(text) + 1); paragraphs[0] = 5; paragraphs[18] = 2
    disk = fixture(work, [('find.bwr', native(text, style, paragraphs)),
                          ('large.bwr', native(b'x' * 32768))])
    captures = []
    with WriterSession(build, 'writer-search', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = WriterCheck(session, build)
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.launch('find.bwr'); original = check.content()
        session.key('alt-left')
        assert check.owner()['w'] == 422
        session.key('ctrl-h'); session.text('alpha'); session.key('tab'); session.text('beta')
        session.key('ctrl-shift-ret')
        replaced = check.content()
        assert replaced['text'] == b'beta beta beta\nomega'
        assert replaced['style'][:4] == bytes([1]) * 4
        assert replaced['style'][5:9] == bytes([2]) * 4
        assert replaced['style'][10:14] == bytes([4]) * 4
        assert replaced['paragraph'][0] == 5 and replaced['paragraph'][15] == 2
        session.wait(lambda: check.state()['words'] == 4, 'live word count')
        captures.append(str(session.screenshot('writer-find-replace-minimum.png')))
        session.key('ctrl-z'); assert check.content() == original
        session.key('ctrl-y'); assert check.content() == replaced
        session.key('ctrl-z'); assert check.content() == original

        # Actual Aa mouse toggle changes which case variants Replace All touches.
        w = check.owner(); client_y = w['y'] + 33; client_h = w['h'] - 34
        check.click(w['x'] + 1 + 130, client_y + client_h - 26 - 92 + 76)
        session.key('ctrl-shift-ret')
        assert check.content()['text'] == b'beta ALPHA beta\nomega'
        session.key('ctrl-z'); assert check.content() == original
        session.key('f3'); first = check.document()
        assert first['anchor'] == 0 and first['caret'] == 5
        session.key('f3'); second = check.document()
        assert second['anchor'] == 12 and second['caret'] == 17
        session.key('f3'); assert check.document()['anchor'] == 0
        session.key('shift-f3'); assert check.document()['anchor'] == 12
        session.key('esc')

        # Edit > Replace reaches the Writer bar rather than the plain Editor.
        session.key('f10'); session.key('right')
        for _ in range(6):
            if check.o.integer('menu_sel') == 4:
                break
            session.key('down')
        assert check.o.integer('menu_sel') == 4
        session.key('ret'); session.key('ctrl-a'); session.text('omega')
        session.key('tab'); session.text('OMEGA'); session.key('ctrl-shift-ret')
        assert check.content()['text'] == b'alpha ALPHA alpha\nOMEGA'
        session.key('ctrl-z'); assert check.content() == original
        session.key('esc')

        # A valid maximum document remains byte/style/history exact on overflow.
        session.launch('large.bwr'); maximum = check.content()
        before_history = (check.state()['first'], check.state()['current'], check.state()['count'])
        session.key('ctrl-h'); session.key('ctrl-a'); session.text('x'); session.key('tab'); session.text('xx')
        session.key('ctrl-shift-ret')
        assert check.content() == maximum
        assert (check.state()['first'], check.state()['current'], check.state()['count']) == before_history
        captures.append(str(session.screenshot('writer-replace-capacity-preserved.png')))
    result = {'passed': True, 'screenshots': captures,
              'checks': ['420-pixel client layout', 'style-preserving case-insensitive Replace All',
                         'single undo/redo operation', 'mouse case toggle', 'F3 forward/backward wrap',
                         'Edit menu routing', 'live word count', 'atomic maximum-document rejection']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
