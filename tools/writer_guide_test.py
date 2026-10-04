"""Open the original packaged Writer quick start in the normal desktop."""
import argparse
import json
import pathlib
import tempfile

from make_writer_example import document
from writer_input_test import WriterSession, WriterCheck, fixture, decode_native

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(build):
    expected = document()
    assert (ROOT / 'assets/examples/writer-guide.bwr').read_bytes() == expected
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-guide-data-'))
    disk = fixture(work, [('Writer guide.bwr', expected)])
    with WriterSession(build, 'writer-guide', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as guest:
        guest.boot(); check = WriterCheck(guest, build)
        guest.launch('Writer guide.bwr')
        guest.wait(lambda: check.state()['file'] >= 0, 'packaged Writer guide opened')
        assert check.content() == decode_native(expected)
        assert check.document()['revision'] == check.state()['saved_revision']
        assert not check.state()['failed_save'] and not check.state()['binding_conflict']
        shot = guest.screenshot('writer-quick-start.png')
        guest.key('ctrl-end'); guest.key('ctrl-home')
        assert check.content() == decode_native(expected)
    result = {'passed': True, 'native_bytes': len(expected), 'screenshot': str(shot),
              'checks': ['reproducible original guide', 'production native decoder',
                         'exact text and styles', 'clean document navigation']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
