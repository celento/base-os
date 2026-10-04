"""Normal Writer replacement, clipboard ownership and failed-save UI flows."""
import argparse
import hashlib
import json
import pathlib
import tempfile

from volume import DATA_LAYOUT, load, resolve
from writer_input_test import WriterSession, WriterCheck, fixture, native, decode_native, WRITER


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-lifecycle-data-'))
    original = b'Original report.'
    disk = fixture(work, [('other.bwr', native(original, bytes([1]) * (len(original) + 1)))])
    pictures = []
    with WriterSession(build, 'writer-lifecycle', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = WriterCheck(session, build)
        session.launch('writer'); session.text('Draft awaiting safe save')
        draft = check.content()
        session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('wrong.txt')
        assert check.o.integer('name_dlg') and check.o.integer('name_failed')
        assert check.content() == draft
        pictures.append(str(session.screenshot('writer-failed-name-keeps-draft.png')))
        session.key('esc')
        assert not check.o.integer('name_dlg') and check.o.integer('edit_close_owner') == -1
        session.key('ctrl-s'); check.name('other.bwr')
        assert check.o.integer('name_failed') and check.content() == draft
        nodes = load(disk.read_bytes())[2]
        assert decode_native(nodes[resolve(nodes, '/Documents/other.bwr')]['data'])['text'] == original
        session.key('esc')

        check.open_file(disk, '/Documents/other.bwr'); check.modal(); check.choose('cancel')
        assert check.content() == draft
        check.open_file(disk, '/Documents/other.bwr'); check.modal(); check.choose('discard')
        assert check.content()['text'] == original
        old_id = check.state()['file']
        def exists(path):
            try:
                return resolve(load(disk.read_bytes())[2], path) >= 0
            except ValueError:
                return False
        session.launch('terminal'); session.text('rm /Documents/other.bwr'); session.key('ret')
        session.wait(lambda: not exists('/Documents/other.bwr'), 'original file removal committed')
        session.text('touch /Documents/reused.txt'); session.key('ret')
        session.wait(lambda: exists('/Documents/reused.txt'), 'replacement file committed')
        session.wait(lambda: check.o.integer('fs_touched') == 0, 'replacement fixture synchronized')
        nodes = load(disk.read_bytes())[2]
        assert resolve(nodes, '/Documents/reused.txt') == old_id
        session.key('ctrl-w'); session.launch('writer')
        session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('rescued.bwr')
        session.wait(lambda: not any(w['open'] and w['kind'] == WRITER for w in check.o.windows()), 'rescued document closed')
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/Documents/reused.txt')]['data'] == b''
        assert decode_native(nodes[resolve(nodes, '/Documents/rescued.bwr')]['data'])['text'] == original

        # Identical plain bytes copied by Editor invalidate the private rich copy.
        session.launch('writer'); session.text('same'); session.key('ctrl-a'); session.key('ctrl-b'); session.key('ctrl-c')
        session.key('ctrl-n'); check.modal(); check.choose('discard')
        session.launch('editor'); session.text('same'); session.key('ctrl-a'); session.key('ctrl-c')
        session.key('ctrl-w'); session.wait(lambda: check.o.integer('edit_close_dlg') == 1, 'Editor close guard')
        check.choose('discard')
        session.launch('writer'); session.key('ctrl-v')
        assert check.content()['text'] == b'same' and check.content()['style'][:4] == bytes(4)
        dirty_revision = check.document()['revision']
        session.key('ctrl-shift-e'); check.name('shared.rtf')
        session.wait(lambda: check.o.integer('name_dlg') == 0, 'separate export completed')
        assert check.document()['revision'] == dirty_revision
        assert check.state()['saved_revision'] != dirty_revision and check.state()['file'] == -1

        # Save before Open completes the original action only after successful Save As.
        check.open_file(disk, '/Documents/rescued.bwr'); check.modal(); check.choose('save'); check.name('converted.bwr')
        session.wait(lambda: check.o.integer('name_dlg') == 0, 'Save-before-Open completed')
        assert check.content()['text'] == original
        nodes = load(disk.read_bytes())[2]
        assert decode_native(nodes[resolve(nodes, '/Documents/converted.bwr')]['data'])['text'] == b'same'
        session.key('ctrl-n'); assert check.content()['text'] == b''
        session.text('New action saves first')
        session.key('ctrl-n'); check.modal(); check.choose('save'); check.name('before-new.bwr')
        assert check.content()['text'] == b''
        nodes = load(disk.read_bytes())[2]
        assert decode_native(nodes[resolve(nodes, '/Documents/before-new.bwr')]['data'])['text'] == b'New action saves first'
        pictures.append(str(session.screenshot('writer-safe-new-completed.png')))

    # Supported recovery mode on an unrecognized disposable disk never writes it.
    unknown = work / 'unrecognized-data.img'
    data = bytearray(DATA_LAYOUT.sectors * 512)
    label = b'Ordinary unformatted test volume!'
    data[:len(label)] = label
    unknown.write_bytes(data)
    before = hashlib.sha256(data).hexdigest()
    with WriterSession(build, 'writer-read-only', extra=['-drive', f'file={unknown},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = WriterCheck(session, build)
        session.launch('writer'); session.text('Keep me after a failed disk save')
        expected = check.content()
        session.key('ctrl-w'); check.modal(); check.choose('save'); check.name('recovery.bwr')
        session.wait(lambda: check.o.integer('name_failed') == 1, 'disk failure was reported')
        assert check.o.integer('name_dlg') and check.owner()['open'] and check.content() == expected
        assert check.state()['failed_save']
        pictures.append(str(session.screenshot('writer-disk-failure-keeps-work.png')))
        session.key('esc'); session.key('ctrl-w'); check.modal(); check.choose('cancel')
        assert check.content() == expected
    assert hashlib.sha256(unknown.read_bytes()).hexdigest() == before
    result = {'passed': True, 'screenshots': pictures, 'disk': str(disk),
              'checks': ['invalid Save As and collision preserve work', 'Open cancellation',
                         'clean orphan identity recovery', 'same-bytes external clipboard ownership',
                         'export leaves dirty native state', 'Save before Open/New',
                         'failed disk sync keeps the document and unknown disk unchanged']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
