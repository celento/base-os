"""Verify recovery and Save conflicts using valid native documents and APIs.

A normal C app changes a file while Writer is open. Separately, the host changes
only stopped disposable volumes. Recovered drafts must never overwrite changed
sources; matching original sources remain safely bound.
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

from volume import load, resolve, commit
from writer_input_test import WriterSession, WriterCheck, fixture, native, decode_native

ROOT = pathlib.Path(__file__).resolve().parents[1]


def contents(disk, path):
    nodes = load(disk.read_bytes())[2]
    return nodes[resolve(nodes, path)]['data']


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-writer-binding-data-'))
    saved = native('Original edition.')
    changed = native('Replaced edition.')
    assert len(saved) == len(changed)
    source = work / 'change.c'
    source.write_text('#include "baseos.h"\nstatic const unsigned char bytes[]={' +
                      ','.join(map(str, changed)) + '};\nint main(void){\n'
                      'if(bos_replace_file("/Documents/report.bwr",bytes,sizeof bytes)!=(int)sizeof bytes)return 1;\n'
                      'if(bos_sync())return 2;\n'
                      'bos_print("Source replacement synchronized.\\n");\nreturn 0;}\n')
    app = work / 'change.bex'
    subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'), str(source), str(app)], check=True)
    disk = fixture(work, [('report.bwr', saved), ('change.bex', app.read_bytes())])
    shots = []
    with WriterSession(build, 'writer-binding-live', extra=['-drive', f'file={disk},format=raw,index=0,if=ide']) as session:
        print(session.directory, flush=True)
        session.boot(); check = WriterCheck(session, build)
        session.launch('report.bwr'); session.key('ctrl-end'); session.text(' plus unsaved changes')
        draft = check.content()
        def recovery_ready():
            try:
                return decode_native(contents(disk, '/prefs/writer-draft.bwr')) == draft and len(contents(disk, '/prefs/writer-binding')) == 24
            except ValueError:
                return False
        session.wait(recovery_ready, 'versioned recovery and source fingerprint', 30)
        magic, version, valid, length, hash_a, hash_b = struct.unpack('<6I', contents(disk, '/prefs/writer-binding'))
        assert (magic, version, valid, length, hash_a, hash_b) == (0x31425257, 1, 1, len(saved), fnv(saved), zlib.crc32(saved))
        baseline = disk.read_bytes()
        session.launch('terminal'); session.text('exec /Documents/change.bex'); session.key('ret')
        session.wait(lambda: contents(disk, '/Documents/report.bwr') == changed, 'ordinary app replacement synchronized')
        session.launch('writer'); session.key('ctrl-s')
        session.wait(lambda: check.o.integer('name_dlg') == 1, 'same-ID changed source requires Save As')
        assert check.o.integer('name_failed') and check.content() == draft
        assert contents(disk, '/Documents/report.bwr') == changed
        shots.append(str(session.screenshot('writer-live-source-conflict.png')))
        session.key('esc'); assert check.content() == draft

    results = []
    for mode in ('matching', 'changed', 'missing-binding'):
        test_disk = work / (mode + '.img'); test_disk.write_bytes(baseline)
        if mode != 'matching':
            slot, generation, nodes = load(baseline)
            if mode == 'changed':
                nodes[resolve(nodes, '/Documents/report.bwr')]['data'] = changed
            else:
                del nodes[resolve(nodes, '/prefs/writer-binding')]
            commit(test_disk, baseline, slot, generation, nodes)
        with WriterSession(build, 'writer-binding-' + mode,
                           extra=['-drive', f'file={test_disk},format=raw,index=0,if=ide']) as session:
            print(session.directory, flush=True)
            session.boot(); check = WriterCheck(session, build)
            assert check.content() == draft
            if mode == 'matching':
                assert check.state()['file'] >= 0
                session.key('ctrl-s')
                session.wait(lambda: decode_native(contents(test_disk, '/Documents/report.bwr')) == draft,
                             'matching baseline saves to its original file')
                assert not check.o.integer('name_dlg')
            else:
                assert check.state()['file'] == -1
                before = contents(test_disk, '/Documents/report.bwr')
                session.key('ctrl-s'); session.wait(lambda: check.o.integer('name_dlg') == 1, 'unbound draft needs new name')
                assert contents(test_disk, '/Documents/report.bwr') == before
                check.name('recovered.bwr')
                session.wait(lambda: check.o.integer('name_dlg') == 0, 'recovered copy saved')
                assert contents(test_disk, '/Documents/report.bwr') == before
                assert decode_native(contents(test_disk, '/Documents/recovered.bwr')) == draft
            shots.append(str(session.screenshot('writer-recovery-' + mode + '.png')))
            results.append(mode)
    result = {'passed': True, 'checks': ['live same-ID same-size content conflict', *results], 'screenshots': shots}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
