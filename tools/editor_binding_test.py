"""Verify plain Editor source conflicts through normal production PS/2 input.

Uses ordinary concurrent Editors, a small native app launched from Terminal,
read-only ELF/DWARF observations, real reboots, and stopped disposable volumes.
No test kernel, injected guest calls, memory modification, fault probe or fuzzing.
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

from elf_debug import DebugInfo
from file_clipboard_input_test import FilesCheck
from volume import load, resolve, commit
from writer_input_test import WriterSession, fixture

ROOT = pathlib.Path(__file__).resolve().parents[1]


def contents(disk, path):
    nodes = load(disk.read_bytes())[2]
    return nodes[resolve(nodes, path)]['data']


def fingerprint(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return len(data), value, zlib.crc32(data)


class EditorCheck(FilesCheck):
    def __init__(self, session, build):
        super().__init__(session, build)
        debug = DebugInfo(pathlib.Path(build) / 'kernel.elf')
        self.doc_size, self.doc_fields = debug.structure('Document', 'src/kernel.c')
        self.source_size, self.source_fields = debug.structure('EditorSource', 'src/kernel.c')
        assert self.doc_size == 65572 and self.source_size == 16

    def doc(self, slot=None):
        slot = self.front()['slot'] if slot is None else slot
        assert 0 <= slot < 8
        address = self.s.layout['EDITOR_BASE'] + slot * self.size + self.fields['doc']
        raw = self.s.memory(address, self.doc_size)
        result = {name: struct.unpack_from('<i', raw, offset)[0]
                  for name, offset in self.doc_fields.items() if name != 'buf'}
        assert 0 <= result['len'] < 65536
        result['text'] = raw[:result['len']]
        return result

    def source(self, slot=None):
        slot = self.front()['slot'] if slot is None else slot
        address = self.s.layout['EDITOR_BASE'] + slot * self.size + self.fields['editor_source']
        raw = self.s.memory(address, self.source_size)
        return struct.unpack('<4I', raw)

    def source_message(self):
        address = self.ui.o.integer('name_failure_message')
        return self.s.memory(address, 80).split(b'\0', 1)[0] if address else b''

    def editor(self):
        return next(w for w in self.ui.o.windows() if w['open'] and w['kind'] == 5)['slot']


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-editor-binding-data-'))
    saved, changed = b'Original edition.', b'Replaced edition.'
    assert len(saved) == len(changed)
    app_source = work / 'change.c'
    app_source.write_text('#include "baseos.h"\nint main(void){\n'
                          'const char bytes[]="Replaced edition.";\n'
                          'if(bos_replace_file("/Documents/external.txt",bytes,sizeof bytes-1)!=(int)sizeof bytes-1)return 1;\n'
                          'if(bos_sync())return 2;\n'
                          'bos_print("Source replacement synchronized.\\n");return 0;}\n')
    app = work / 'change.bex'
    subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'), str(app_source), str(app)], check=True)
    disk = fixture(work, [('report.txt', saved), ('external.txt', saved), ('change.bex', app.read_bytes())])
    shots = []
    with WriterSession(build, 'editor-binding-live', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = EditorCheck(session, build); ui = check.ui
        session.launch('settings'); session.key('1'); session.key('ret'); session.key('ctrl-w')
        session.launch('report.txt'); first = check.front()['slot']; session.key('ctrl-end'); session.text(' first')
        session.launch('report.txt'); second = check.front()['slot']; session.key('ctrl-end'); session.text(' second')
        assert first != second and check.doc(first)['file'] == check.doc(second)['file']
        second_draft = check.doc(second)['text']
        check.focus(first); session.key('ctrl-s')
        session.wait(lambda: check.doc(first)['saved_ok'], 'first Editor save synchronized')
        first_saved = check.doc(first)['text']
        assert contents(disk, '/Documents/report.txt') == first_saved
        check.focus(second); session.key('ctrl-s')
        session.wait(lambda: ui.o.integer('name_dlg') and ui.o.integer('name_failed'), 'second Editor conflict opens Save As')
        assert check.doc(second)['text'] == second_draft and not check.doc(second)['saved_ok']
        assert b'Source changed' in check.source_message()
        ui.name('report.txt')
        assert ui.o.integer('name_dlg') and ui.o.integer('name_failed')
        assert contents(disk, '/Documents/report.txt') == first_saved
        shots.append(str(session.screenshot('two-editor-source-conflict.png')))
        ui.name('second-copy.txt')
        session.wait(lambda: not ui.o.integer('name_dlg'), 'conflicting second draft saved as a new file')
        assert contents(disk, '/Documents/second-copy.txt') == second_draft
        assert contents(disk, '/Documents/report.txt') == first_saved
        session.key('ctrl-w')
        check.focus(first); baseline = check.source(first)
        session.key('ctrl-z'); undone = check.doc(first)['text']
        assert undone != first_saved and check.source(first) == baseline
        session.key('ctrl-s'); session.wait(lambda: check.doc(first)['saved_ok'], 'undo keeps latest source baseline')
        assert not ui.o.integer('name_dlg') and contents(disk, '/Documents/report.txt') == undone
        session.key('ctrl-w')

        session.launch('external.txt'); owner = check.front()['slot']; session.key('ctrl-end'); session.text(' unsaved draft')
        draft = check.doc(owner)['text']
        def recovery_ready():
            try:
                sidecar = contents(disk, '/prefs/editor-bindings')
                record = struct.unpack_from('<7I', sidecar, 24 + owner * 28)
                return (len(sidecar) == 248 and struct.unpack_from('<3I', sidecar) == (0x31424445, 1, 8)
                        and record == (1, *fingerprint(saved), *fingerprint(draft))
                        and contents(disk, f'/prefs/draft{owner}.txt') == draft
                        and ui.o.integer('fs_touched') == 0)
            except (ValueError, struct.error):
                return False
        session.wait(recovery_ready, 'paired v1 session, complete draft and source fingerprints', 30)
        recovery_disk = disk.read_bytes()
        source_id = check.doc(owner)['file']; identity = check.doc(owner)['identity']
        session.launch('terminal'); session.text('exec /Documents/change.bex'); session.key('ret')
        session.wait(lambda: contents(disk, '/Documents/external.txt') == changed, 'native app same-size replacement synchronized')
        assert resolve(load(disk.read_bytes())[2], '/Documents/external.txt') == source_id
        session.key('ctrl-w'); check.focus(owner); session.key('ctrl-s')
        session.wait(lambda: ui.o.integer('name_dlg') and ui.o.integer('name_failed'), 'native replacement conflict detected')
        assert check.doc(owner)['identity'] == identity and check.doc(owner)['text'] == draft
        assert contents(disk, '/Documents/external.txt') == changed
        shots.append(str(session.screenshot('editor-terminal-source-conflict.png')))
        ui.name('external.txt'); assert ui.o.integer('name_dlg') and b'Source changed' in check.source_message()
        ui.name('external-copy.txt'); session.wait(lambda: not ui.o.integer('name_dlg'), 'external conflict recovered into a new file')
        assert contents(disk, '/Documents/external-copy.txt') == draft
        assert contents(disk, '/Documents/external.txt') == changed

    recovery_cases = []
    for mode in ('matching', 'changed-source', 'missing-binding', 'changed-draft'):
        test_disk = work / (mode + '.img'); test_disk.write_bytes(recovery_disk)
        expected_draft = draft
        if mode != 'matching':
            slot, generation, nodes = load(recovery_disk)
            if mode == 'changed-source':
                nodes[resolve(nodes, '/Documents/external.txt')]['data'] = changed
            elif mode == 'missing-binding':
                del nodes[resolve(nodes, '/prefs/editor-bindings')]
            else:
                expected_draft = b'Another complete recovered draft.'
                nodes[resolve(nodes, f'/prefs/draft{owner}.txt')]['data'] = expected_draft
            commit(test_disk, recovery_disk, slot, generation, nodes)
        with WriterSession(build, 'editor-binding-' + mode,
                           extra=['-drive', f'file={test_disk},format=raw,index=0,if=ide']) as session:
            print(session.directory, flush=True)
            session.boot(); check = EditorCheck(session, build); ui = check.ui
            restored = check.editor(); check.focus(restored)
            assert check.doc(restored)['text'] == expected_draft and not check.doc(restored)['saved_ok']
            if mode == 'matching':
                assert check.doc(restored)['file'] >= 0 and check.source(restored) == (1, *fingerprint(saved))
                session.key('ctrl-s'); session.wait(lambda: check.doc(restored)['saved_ok'], 'unchanged source accepts recovered draft')
                assert not ui.o.integer('name_dlg') and contents(test_disk, '/Documents/external.txt') == expected_draft
            else:
                assert check.doc(restored)['file'] == -1 and not check.source(restored)[0]
                original = contents(test_disk, '/Documents/external.txt')
                session.key('ctrl-s'); session.wait(lambda: ui.o.integer('name_dlg'), 'unbound recovered draft requires new filename')
                ui.name('external.txt'); assert ui.o.integer('name_dlg') and ui.o.integer('name_failed')
                assert contents(test_disk, '/Documents/external.txt') == original
                ui.name('recovered.txt'); session.wait(lambda: not ui.o.integer('name_dlg'), 'unbound recovery saved separately')
                assert contents(test_disk, '/Documents/recovered.txt') == expected_draft
                assert contents(test_disk, '/Documents/external.txt') == original
            shots.append(str(session.screenshot('editor-recovery-' + mode + '.png')))
            recovery_cases.append(mode)

    unknown = work / 'unknown.img'; unknown.write_bytes(bytes(16 * 1024 * 1024))
    with WriterSession(build, 'editor-binding-readonly', extra=['-drive', f'file={unknown},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = EditorCheck(session, build); ui = check.ui
        session.launch('editor'); owner = check.front()['slot']; session.text('Keep this draft')
        session.key('ctrl-s'); ui.name('unsynced.txt')
        assert ui.o.integer('name_dlg') and ui.o.integer('name_failed') and not check.doc(owner)['saved_ok']
        owned = check.doc(owner)['file']; baseline = check.source(owner)
        assert owned >= 0 and baseline == (1, *fingerprint(check.doc(owner)['text']))
        ui.name('unsynced.txt')
        assert check.doc(owner)['file'] == owned and check.source(owner) == baseline and not check.source_message()
        session.key('esc'); session.key('ctrl-z'); undone = check.doc(owner)['text']
        assert check.source(owner) == baseline
        session.key('ctrl-s')
        assert not ui.o.integer('name_dlg') and check.doc(owner)['file'] == owned and not check.doc(owner)['saved_ok']
        assert check.source(owner) == (1, *fingerprint(undone))
        session.key('ctrl-s'); assert not ui.o.integer('name_dlg') and not check.doc(owner)['saved_ok']
        shots.append(str(session.screenshot('editor-safe-failed-sync-retries.png')))
    assert unknown.read_bytes() == bytes(16 * 1024 * 1024)
    result = {'passed': True, 'checks': ['two Editors share one source safely', 'owned-name conflict rejected',
              'Save As preserves both versions', 'undo does not roll back source baseline',
              'Terminal/native app same-ID same-size replacement', *recovery_cases,
              'read-only Save As and direct retries', 'unknown volume unchanged'], 'screenshots': shots}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
