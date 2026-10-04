"""Exercise Files Cut/Copy/Paste and text-clipboard separation through PS/2."""
import argparse
import json
import pathlib
import struct
import tempfile

from elf_debug import DebugInfo
from volume import load, resolve, commit
from writer_input_test import WriterSession, WriterCheck, fixture, native


class FilesCheck:
    def __init__(self, session, build):
        self.s = session; self.ui = WriterCheck(session, build)
        self.size, self.fields = DebugInfo(pathlib.Path(build) / 'kernel.elf').structure('WindowState', 'src/kernel.c')
        assert self.fields['doc'] < 4096 and self.size * 8 < session.layout['EDITOR_CAPACITY']

    def front(self):
        return max((w for w in self.ui.o.windows() if w['open'] and not w['min']), key=lambda w: w['z'])

    def state(self, slot=None):
        slot = self.front()['slot'] if slot is None else slot
        raw = self.s.memory(self.s.layout['EDITOR_BASE'] + slot * self.size, self.fields['doc'])
        result = {key: struct.unpack_from('<i', raw, self.fields[key])[0]
                  for key in ('cwd', 'selected', 'first', 'count')}
        result['ids'] = struct.unpack_from('<256i', raw, self.fields['ids'])[:result['count']]
        index = result['selected'] - int(result['cwd'] != 0)
        result['selected_id'] = result['ids'][index] if 0 <= index < len(result['ids']) else -1
        return result

    def focus(self, slot):
        if self.front()['slot'] == slot:
            return
        self.s.wait(lambda: self.ui.o.integer('tb_n') == sum(w['open'] for w in self.ui.o.windows()),
                    'taskbar reflects open windows')
        count=self.ui.o.integer('tb_n')
        ids=struct.unpack('<8i',self.ui.o.read('tb_id'))[:count]
        index=ids.index(slot)
        xs=struct.unpack('<8i',self.ui.o.read('tb_x'))
        widths=struct.unpack('<8i',self.ui.o.read('tb_w'))
        self.ui.click(xs[index]+widths[index]//2,self.ui.o.integer('fb_h')-22)
        self.s.wait(lambda:self.front()['slot']==slot,'requested taskbar window is focused',10)

    def clipboard(self, key):
        before = self.ui.o.integer('files_message_until')
        self.s.key(key)
        self.s.wait(lambda: self.ui.o.integer('files_message_until') != before,
                    'file clipboard operation completed')

    def select(self, ident):
        for _ in range(257):
            if self.state()['selected_id'] == ident:
                return
            self.s.key('down')
        raise AssertionError('File was not selectable')

    def root(self):
        for _ in range(64):
            if self.state()['cwd'] == 0:
                return
            self.s.key('backspace')
        raise AssertionError('Could not navigate to root')

    def folder(self, disk, path):
        self.root()
        nodes = load(disk.read_bytes())[2]; prefix = ''
        for component in path.strip('/').split('/'):
            if not component:
                continue
            prefix += '/' + component
            self.select(resolve(nodes, prefix)); self.s.key('ret')


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-file-clipboard-input-data-'))
    binary = bytes(range(256)) * 257
    disk = fixture(work, [('note.txt', b'Copy this document exactly.\n'), ('binary.dat', binary)])
    data = disk.read_bytes(); slot, generation, nodes = load(data)
    for ident, parent, name, directory, content in [(4, 0, 'Target', 1, b''), (5, 0, 'Moved', 1, b''),
                                                   (6, 1, 'Folder', 1, b''), (7, 6, 'child.txt', 0, b'A nested document.\n'),
                                                   (8, 1, 'rich.bwr', 0, native('Native copy document'))]:
        assert ident not in nodes
        nodes[ident] = dict(parent=parent, name=name, directory=directory, app=0, data=content, modified=0)
    commit(disk, data, slot, generation, nodes)
    captures = []
    with WriterSession(build, 'file-clipboard-input', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = FilesCheck(session, build)
        session.launch('files'); owner = check.front()['slot']
        check.folder(disk, '/Documents'); check.select(2)
        check.clipboard('ctrl-c'); check.folder(disk, '/Target'); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]
        copied = check.state()['selected_id']
        assert nodes[copied]['parent'] == 4 and nodes[copied]['data'] == nodes[2]['data']
        check.clipboard('ctrl-v'); nodes = load(disk.read_bytes())[2]
        duplicate = check.state()['selected_id']
        assert duplicate != copied and nodes[duplicate]['data'] == nodes[2]['data']
        captures.append(str(session.screenshot('files-copy-collision-safe.png')))

        check.folder(disk, '/Documents'); check.select(2); check.clipboard('ctrl-x')
        captures.append(str(session.screenshot('files-cut-marked.png')))
        check.folder(disk, '/Target'); before = disk.read_bytes(); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]
        assert nodes[2]['parent'] == 1 and nodes[copied]['data'] == nodes[2]['data']
        assert check.ui.o.integer('clip_len') == 0
        captures.append(str(session.screenshot('files-cut-collision-preserved.png')))
        check.folder(disk, '/Moved'); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]
        assert nodes[2]['parent'] == 5 and check.state()['selected_id'] == 2
        check.clipboard('ctrl-x'); check.clipboard('ctrl-v')  # Same folder is a no-op.
        assert load(disk.read_bytes())[2][2]['parent'] == 5

        check.folder(disk, '/Documents'); check.select(6); check.clipboard('ctrl-c')
        check.folder(disk, '/Target'); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]; tree = check.state()['selected_id']
        assert nodes[tree]['directory'] and nodes[tree]['parent'] == 4
        assert any(n['parent'] == tree and n['data'] == b'A nested document.\n' for n in nodes.values())
        check.folder(disk, '/Documents'); check.select(6); check.clipboard('ctrl-x'); session.key('ret'); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]; assert nodes[6]['parent'] == 1 and nodes[7]['parent'] == 6

        # The Edit menu mouse/keyboard route copies an ordinary large binary file.
        check.folder(disk, '/Documents'); check.select(3)
        session.key('f10'); session.key('right'); session.key('down'); session.key('ret')
        check.folder(disk, '/Target'); check.clipboard('ctrl-v')
        nodes = load(disk.read_bytes())[2]
        assert nodes[check.state()['selected_id']]['data'] == binary

        # Both Paste and File > Duplicate retain native document associations.
        check.folder(disk, '/Documents'); check.select(8); check.clipboard('ctrl-c'); check.clipboard('ctrl-v')
        rich_copy=check.state()['selected_id']; nodes=load(disk.read_bytes())[2]
        assert rich_copy!=8 and nodes[rich_copy]['name'].endswith('.bwr')
        session.key('ret'); assert check.ui.content()['text']==b'Native copy document'
        session.key('ctrl-w'); check.focus(owner); check.select(8)
        session.key('f10')
        for _ in range(8):
            if check.ui.o.integer('menu_sel')==5: break
            session.key('down')
        assert check.ui.o.integer('menu_sel')==5
        session.key('ret')
        session.wait(lambda: check.state()['selected_id']!=8 and check.ui.o.integer('fs_touched')==0,
                     'Duplicate synchronized')
        duplicate_rich=check.state()['selected_id']; nodes=load(disk.read_bytes())[2]
        assert duplicate_rich not in (8,rich_copy) and nodes[duplicate_rich]['name'].endswith('.bwr')
        session.key('ret'); assert check.ui.content()['text']==b'Native copy document'
        session.key('ctrl-w'); check.focus(owner)

        # A deleted source's reused numeric ID cannot make a later paste target it.
        check.folder(disk, '/Moved'); check.select(2); check.clipboard('ctrl-c')
        session.launch('terminal'); session.text('rm /Moved/note.txt'); session.key('ret')
        def exists(path):
            try:
                return resolve(load(disk.read_bytes())[2], path) >= 0
            except ValueError:
                return False
        session.wait(lambda: not exists('/Moved/note.txt'), 'source deletion synchronized')
        session.text('touch /Moved/reused.txt'); session.key('ret')
        session.wait(lambda: exists('/Moved/reused.txt'), 'reused source ID synchronized')
        assert resolve(load(disk.read_bytes())[2], '/Moved/reused.txt') == 2
        session.key('ctrl-w'); check.focus(owner)
        before_children = {i for i,n in load(disk.read_bytes())[2].items() if n['parent'] == 5}
        check.clipboard('ctrl-v'); nodes = load(disk.read_bytes())[2]
        assert {i for i,n in nodes.items() if n['parent'] == 5} == before_children and nodes[2]['data'] == b''

        # File clipboard ownership cannot delete a Writer selection or paste stale text.
        session.launch('writer'); session.text('Keep this selected text'); session.key('ctrl-a')
        writer_before = check.ui.document(); session.key('ctrl-v')
        assert check.ui.document() == writer_before
        session.key('ctrl-c')  # New plain text clears the file clipboard.
        check.focus(owner)
        before = {i for i,n in load(disk.read_bytes())[2].items() if n['parent'] == 5}
        check.clipboard('ctrl-v')
        assert {i for i,n in load(disk.read_bytes())[2].items() if n['parent'] == 5} == before
        captures.append(str(session.screenshot('files-text-clipboard-is-distinct.png')))
        session.launch('editor'); session.key('ctrl-v')
        front = check.front(); assert front['kind'] == 5
        debug = DebugInfo(build / 'kernel.elf'); doc_size, doc_fields = debug.structure('Document', 'src/kernel.c')
        raw = session.memory(session.layout['EDITOR_BASE'] + front['slot'] * check.size + check.fields['doc'], doc_size)
        length = struct.unpack_from('<i', raw, doc_fields['len'])[0]
        assert raw[:length] == b'Keep this selected text'
    with WriterSession(build, 'file-clipboard-reboot', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        session.boot(); nodes = load(disk.read_bytes())[2]
        assert nodes[copied]['data'] == b'Copy this document exactly.\n'
        assert nodes[duplicate]['data'] == nodes[copied]['data']
        assert nodes[2]['data'] == b'' and nodes[2]['name'] == 'reused.txt'
        assert any(n['parent'] == tree and n['data'] == b'A nested document.\n' for n in nodes.values())
    result = {'passed': True, 'screenshots': captures,
              'checks': ['copy and collision-safe duplicate', 'Cut marker', 'collision-preserving Cut',
                         'identity-preserving move and same-folder no-op', 'recursive copy/subtree rejection',
                         'Edit menu route', 'native file associations after Paste and Duplicate', '65792-byte binary copy', 'deleted/reused source rejection',
                         'Writer/Editor text clipboard separation', 'reboot persistence']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
