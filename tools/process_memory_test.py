"""Ordinary C1/C2b guest accounting and reboot checks on disposable media.

Uses valid frozen BEX1 files plus one public-ABI fixed-workspace client. This is
fixture-kernel evidence, not a production PS/2 desktop test. The caller owns
the serialized QEMU slot. No debugger, sockets, guest-memory reads or fault
probes are used. --prepare-only never launches QEMU. Supplied builds are never
modified and must carry a clean, verified runtime identity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from build_app import build as build_app, tool
from init_data import initialize
from layout import constants
from platform_evidence import provenance
from update_image import install_kernel
import volume

ROOT = Path(__file__).resolve().parents[1]
COMPLETE = b'C1/C2 ordinary guest complete\n'
NOTE = b'This note was saved by a protected C application.\n'
FIELDS = 'total free allocated high_water backing records done'.split()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def freeze(directory):
    folder = ROOT / 'tests/fixtures/bex1-legacy'
    manifest = json.loads((folder / 'manifest.json').read_text())
    files = {}
    for name, expected in manifest['files'].items():
        path = folder / name
        assert path.stat().st_size == expected['bytes'] and sha(path) == expected['sha256'], name
        files[name] = path.read_bytes()
    build_app(ROOT / 'tests/process_memory_app.c', directory / 'process-memory.bex')
    files['process-memory.bex'] = (directory / 'process-memory.bex').read_bytes()
    return files, manifest


def disk(path, profile, files):
    assert not path.exists(), 'Refusing to replace an existing test disk'
    initialize(path, profile=profile)
    nodes = {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0),
        2: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0)}
    for name, data in files.items():
        nodes[len(nodes)] = dict(parent=1, name=name, directory=0, app=0, data=data, modified=1234)
    layout = volume.data_layout(profile)
    header, payload = volume.encode_snapshot(nodes, layout, 1)
    raw = bytearray(path.read_bytes());offset = layout.lbas[0] * 512
    raw[offset:offset + 512] = header.ljust(512, b'\0')
    raw[offset + 512:offset + 512 + len(payload)] = payload
    assert volume.load(raw)[2] == nodes
    path.write_bytes(raw)


def prepare(build, work):
    identity = provenance(build)
    runtime = build.parent
    # The application is built through this checkout's SDK, so establish it is
    # byte-identical to the supplied production build, independently of HEAD.
    for name in ('sdk/baseos.h', 'sdk/baseos_abi.h', 'sdk/start.c', 'sdk/app.ld', 'src/layout.h'):
        assert (ROOT / name).read_bytes() == (runtime / name).read_bytes(), name
    files, frozen = freeze(work)
    guest = ROOT / 'tests/process_memory_guest.c'
    subprocess.run([tool('gcc'), '-std=gnu11', '-Os', '-g', '-Wall', '-Wextra',
                    '-ffreestanding', '-m32', '-fno-pie', '-fno-pic',
                    '-fno-stack-protector', '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-msoft-float', '-I', str(runtime), '-I', str(runtime / 'src'),
                    '-I', str(build), '-c', str(guest), '-o', str(work / 'kernel.o')], check=True)
    objects = [p for p in sorted(build.glob('*.o')) if p.name != 'kernel.o']
    subprocess.run([tool('ld'), '-T', str(build / 'linker.ld'), '-nostdlib', '-m',
                    'elf_i386', '-z', 'noexecstack', '-o', str(work / 'kernel.elf'),
                    str(work / 'kernel.o'), *map(str, objects)], check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(work / 'kernel.elf'),
                    str(work / 'kernel.bin')], check=True)
    c = constants();image = bytearray(c['DISK_SECTORS'] * 512)
    image[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(image, (work / 'kernel.bin').read_bytes(), c)
    (work / 'boot.img').write_bytes(image)
    identity.update(evidence_class='fixture kernel with unchanged production runtime objects',
                    frozen_bex1=frozen,
                    fixture_sources={str(p.relative_to(ROOT)):sha(p) for p in
                                     (guest, ROOT / 'tests/process_memory_app.c', Path(__file__))},
                    fixture_artifacts={name:sha(work / name) for name in
                                       ('kernel.o', 'kernel.elf', 'kernel.bin', 'boot.img', 'process-memory.bex')},
                    production_object_sha256={p.name:sha(p) for p in objects},
                    fixture_runtime_kernel_source_sha256=sha(runtime / 'src/kernel.c'))
    return files, identity


def run(boot, data, work, label, memory, marker, seconds):
    log, errors = work / (label + '.log'), work / (label + '.stderr')
    log.write_text('')
    command = ['qemu-system-i386', '-m', memory, '-vga', 'std', '-boot', 'a',
               '-drive', f'file={boot},format=raw,index=0,if=floppy',
               '-drive', f'file={data},format=raw,index=0,if=ide,cache=writeback',
               '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot']
    started = time.monotonic()
    with errors.open('w') as error_file:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=error_file)
        try:
            while marker not in log.read_text():
                text = log.read_text()
                if 'PANIC:' in text or process.poll() is not None:
                    raise AssertionError(f'{label}: {marker} missing\n{text}\n{errors.read_text()}')
                if time.monotonic() - started > seconds:
                    raise AssertionError(f'{label}: marker deadline\n{text}\n{errors.read_text()}')
                time.sleep(.1)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5)
                raise AssertionError(f'{label}: QEMU did not close normally')
    text = log.read_text();assert 'PANIC:' not in text
    rows = []
    for match in re.finditer(r'PM-COUNT ([\w-]+)' + ''.join(r' ' + f + r'=(\d+)' for f in FIELDS), text):
        rows.append(dict(label=match[1], **dict(zip(FIELDS,map(int,match.groups()[1:])))))
    assert rows, 'Guest did not publish accounting records'
    return dict(command=command, seconds=round(time.monotonic()-started,3),
                log_sha256=sha(log), stderr_sha256=sha(errors), checkpoints=rows)


def validate_files(path):
    nodes = volume.load(path.read_bytes())[2]
    expected = {'/Documents/pm-complete.txt':COMPLETE, '/Documents/sdk-note.txt':NOTE}
    expected.update({f'/Documents/pm-{display}.bin':bytes((i*37+display*13)&255 for i in range(24576))
                     for display in (2,6)})
    for name, content in expected.items():
        assert nodes[volume.resolve(nodes,name)]['data'] == content, name
    return {name:dict(bytes=len(content),sha256=hashlib.sha256(content).hexdigest())
            for name, content in expected.items()}


def main(args):
    build,work = args.build.resolve(),args.work.resolve()
    assert not work.exists() or not any(work.iterdir()), '--work must be new or empty'
    work.mkdir(parents=True,exist_ok=True)
    report={'passed':False,'prepared_only':args.prepare_only,'profiles':{}}
    try:
        files,report['provenance']=prepare(build,work)
        profiles = ('default','large') if args.profile=='both' else (args.profile,)
        for profile in profiles:
            data=work/(profile+'.img');disk(data,profile,files)
            entry={'ram_mib':128 if profile=='large' else 64,'initial_disk_sha256':sha(data)}
            report['profiles'][profile]=entry
            if args.prepare_only:continue
            entry['first']=run(work/'boot.img',data,work,profile, str(entry['ram_mib'])+'M',
                               'PROCESS-MEMORY-FUNCTIONAL-PASS',args.timeout)
            entry['before_reboot_files']=validate_files(data)
            entry['reboot']=run(work/'boot.img',data,work,profile+'-reboot',str(entry['ram_mib'])+'M',
                                'PROCESS-MEMORY-REBOOT-PASS',args.timeout)
            entry['after_reboot_files']=validate_files(data)
            assert entry['before_reboot_files']==entry['after_reboot_files']
            entry['final_disk_sha256']=sha(data)
        report['passed']=not args.prepare_only
    finally:
        (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Prepared only; QEMU not started.' if args.prepare_only else
          'C1/C2b fixture-kernel accounting, valid-app lifecycle, owner cleanup and exact reboot bytes passed.')
    print(work/'report.json')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=Path);parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--profile',choices=('default','large','both'),default='both')
    parser.add_argument('--prepare-only',action='store_true');parser.add_argument('--timeout',type=int,default=180)
    main(parser.parse_args())
