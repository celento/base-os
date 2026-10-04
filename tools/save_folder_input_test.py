"""Normal Save As must not follow a deleted folder's reused numeric slot."""
import argparse
import json
import pathlib
import tempfile

from file_clipboard_input_test import FilesCheck
from volume import load, resolve, commit
from writer_input_test import WriterSession, fixture, native, decode_native


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-save-folder-data-'))
    disk = fixture(work, [])
    data = disk.read_bytes(); slot, generation, nodes = load(data)
    records = [(2, 1, 'TextFolder', 1, b''), (3, 2, 'original.txt', 0, b'Plain original'),
               (4, 1, 'RichFolder', 1, b''), (5, 4, 'original.bwr', 0, native('Rich original')),
               (6, 1, 'Stable', 1, b''), (7, 6, 'stable.txt', 0, b'Stable original'),
               (8, 1, 'replace-text.sh', 0, b'rm /Documents/TextFolder\nmkdir /Documents/ReplacementText\n'),
               (9, 1, 'replace-rich.sh', 0, b'rm /Documents/RichFolder\nmkdir /Documents/ReplacementRich\n'),
               (10, 0, 'plain-rescued.txt', 0, b'Pre-existing root document')]
    for ident, parent, name, directory, content in records:
        nodes[ident] = dict(parent=parent, name=name, directory=directory, app=0, data=content, modified=0)
    commit(disk, data, slot, generation, nodes)
    shots = []
    with WriterSession(build, 'save-folder', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = FilesCheck(session, build); ui = check.ui
        def exists(path):
            try:
                return resolve(load(disk.read_bytes())[2], path) >= 0
            except ValueError:
                return False
        def replace_folder(path, replacement):
            script='replace-text.sh' if path.endswith('TextFolder') else 'replace-rich.sh'
            session.launch('terminal'); session.text('run /Documents/' + script); session.key('ret')
            session.wait(lambda: not exists(path) and exists(replacement), 'folder replacement synchronized')
            session.key('ctrl-w')
        session.launch('original.txt'); editor = check.front()['slot']
        assert check.state(editor)['cwd'] == 2
        session.key('ctrl-end'); session.text(' edited')
        replace_folder('/Documents/TextFolder', '/Documents/ReplacementText')
        nodes = load(disk.read_bytes())[2]; assert nodes[2]['directory'] and nodes[2]['name'] != 'TextFolder'
        session.key('ctrl-s'); session.wait(lambda: ui.o.integer('name_dlg') == 1, 'Editor Save As')
        assert check.state(editor)['cwd'] == 0
        shots.append(str(session.screenshot('editor-reused-folder-root-fallback.png')))
        ui.name('plain-rescued.txt')
        assert ui.o.integer('name_dlg') and ui.o.integer('name_failed')
        assert load(disk.read_bytes())[2][10]['data'] == b'Pre-existing root document'
        ui.name('plain-rescued-2.txt')
        session.wait(lambda: not ui.o.integer('name_dlg'), 'rescued Editor save completed')
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/plain-rescued-2.txt')]['data'] == b'Plain original edited'
        assert not any(n['parent'] == 2 for n in nodes.values())
        session.key('ctrl-w')

        session.launch('original.bwr'); writer = check.front()['slot']
        assert check.state(writer)['cwd'] == 4
        session.key('ctrl-end'); session.text(' styled'); rich = ui.content()
        replace_folder('/Documents/RichFolder', '/Documents/ReplacementRich')
        nodes = load(disk.read_bytes())[2]; assert nodes[4]['directory'] and nodes[4]['name'] != 'RichFolder'
        session.key('ctrl-s'); session.wait(lambda: ui.o.integer('name_dlg') == 1, 'Writer Save As')
        assert check.state(writer)['cwd'] == 0
        shots.append(str(session.screenshot('writer-reused-folder-root-fallback.png')))
        ui.name('rich-rescued.bwr')
        session.wait(lambda: not ui.o.integer('name_dlg'), 'rescued Writer save completed')
        nodes = load(disk.read_bytes())[2]
        assert decode_native(nodes[resolve(nodes, '/rich-rescued.bwr')]['data']) == rich
        assert not any(n['parent'] == 4 for n in nodes.values())
        session.key('ctrl-w')

        # A still-existing folder stays selected, even after its file disappears.
        session.launch('stable.txt'); editor = check.front()['slot']; session.key('ctrl-end'); session.text(' changed')
        session.launch('terminal'); session.text('rm /Documents/Stable/stable.txt'); session.key('ret')
        session.wait(lambda: not exists('/Documents/Stable/stable.txt'), 'file removal synchronized')
        session.key('ctrl-w'); session.key('ctrl-s')
        session.wait(lambda: ui.o.integer('name_dlg') == 1, 'stable-folder Save As')
        assert check.state(editor)['cwd'] == 6
        ui.name('new.txt'); session.wait(lambda: not ui.o.integer('name_dlg'), 'stable-folder save completed')
        assert exists('/Documents/Stable/new.txt') and not exists('/new.txt')
        def saved_folder_context():
            try:
                n=load(disk.read_bytes())[2]
                return b'/Documents/Stable/new.txt\0' in n[resolve(n, '/prefs/session')]['data']
            except ValueError:
                return False
        session.wait(saved_folder_context, 'current folder context saved', 30)
        session.wait(lambda: ui.o.integer('fs_touched') == 0, 'session synchronized')
    with WriterSession(build, 'save-folder-reboot', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        session.boot(); check = FilesCheck(session, build)
        editor = next(w for w in check.ui.o.windows() if w['open'] and w['kind'] == 5)
        assert check.state(editor['slot'])['cwd'] == 6
        nodes = load(disk.read_bytes())[2]
        assert nodes[resolve(nodes, '/Documents/Stable/new.txt')]['data'] == b'Stable original changed'
        shots.append(str(session.screenshot('editor-restored-folder-context.png')))
    result = {'passed': True, 'screenshots': shots,
              'checks': ['Editor reused-folder fallback', 'Writer reused-folder fallback',
                         'unchanged replacement folders', 'unrelated Save As name collision rejected', 'valid folder retained', 'Editor folder restored after reboot']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
