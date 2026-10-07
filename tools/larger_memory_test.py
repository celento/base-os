"""Explicit optional-256MiB ordinary workspace/accounting fixture.

Prepare first, then await the coordinator's exclusive QEMU grant. Preparation
never starts a guest and never changes the supplied production build. The test
relinks only its fixture kernel entry with unchanged identified runtime objects;
production desktop evidence is a separate larger_memory_input_test.py gate.
Only serial output and stopped disposable disks are inspected. There are no
sockets, debugger operations, guest-memory observations or invalid native probes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import address_space_input_test as desktop
from address_space_test import FIELDS

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = 3 * 1024 * 1024
COMPLETE = b'Two ordinary 3 MiB workspaces complete\n'
APP_FIELDS = 'owner display stage checksum steps file operation sync mapped owned tables workspace base stack checks reserved'.split()
CHECKPOINTS = ('baseline', 'frozen-peer', 'one-filled', 'both-filled-barrier',
               'mixed-retained', 'b-closed', 'b-zero-relaunch', 'a-closed',
               'a-zero-relaunch', 'a-normal-exit', 'b-normal-exit', 'all-closed', 'final')
PROVENANCE_FIELDS = ('built_source', 'artifacts', 'runtime_source_sha256',
                     'runtime_helper_sha256', 'production_object_sha256', 'harness_sha256')


def source_identity():
    return {str(path.relative_to(ROOT)): desktop.artifact(path) for path in
            (Path(__file__), ROOT / 'tests/larger_memory_guest.c',
             ROOT / 'tests/larger_memory_fixture_app.c', ROOT / 'tests/test_larger_memory.py',
             ROOT / 'tools/address_space_test.py',
             ROOT / 'tools/address_space_input_test.py', ROOT / 'tests/executable_inspect_host.c')}


def command(work, label, args):
    argv = list(map(str, args))
    (work / (label + '.command.json')).write_text(json.dumps(argv, indent=2) + '\n')
    try:
        result = subprocess.run(argv, capture_output=True, text=True)
    except BaseException as error:
        (work / (label + '.log')).write_text(repr(error) + '\n')
        raise
    (work / (label + '.log')).write_text(result.stdout + result.stderr)
    result.check_returncode()
    return result.stdout


def expected_byte(offset, display):
    page, lane = divmod(offset, 4096)
    if lane < 4:
        word = page ^ ((display * 0x9e3779b9) & 0xffffffff)
        return (word >> (lane * 8)) & 255
    return (offset * 37 + display * 13 + page * 17) & 255


def expected_output(display):
    return bytes(expected_byte(i, display) for i in range(4064, 4064 + 32768))


def expected_checksum(display):
    # Independent arithmetic oracle: replace four old payload bytes per page.
    total = WORKSPACE // 256 * sum(range(256))
    for page in range(WORKSPACE // 4096):
        for lane in range(4):
            offset = page * 4096 + lane
            total += expected_byte(offset, display) - ((offset * 37 + display * 13 + page * 17) & 255)
    return total


def expected_files():
    return {'/Documents/lm-complete.txt': COMPLETE, '/Documents/counter-4.txt': b'20\n',
            '/Documents/as-2.bin': expected_output(2), '/Documents/as-6.bin': expected_output(6),
            '/Documents/source.bin': bytes((i * 29 + 7) & 255 for i in range(8192))}


def prepare(args, runtime, modules):
    work = args.work
    if work.exists() and any(work.iterdir()):
        raise ValueError('Preparation requires a new empty --work directory')
    work.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, status='preparing; no guest started', profiles={},
                  optional_memory_mib=args.memory_mib, fixture_sources=source_identity(),
                  evidence_class='valid ordinary SDK apps; fixture entry with unchanged production objects',
                  plans={})
    report_path = work / 'report.json'
    try:
        report['provenance'] = desktop.source_provenance(args, runtime, modules)
        from build_app import tool
        frozen = ROOT / 'tests/fixtures/bex1-legacy'
        manifest = json.loads((frozen / 'manifest.json').read_text())
        apps = {}
        for name, expected in manifest['files'].items():
            if desktop.artifact(frozen / name) != expected:
                raise ValueError('Frozen BEX1 differs: ' + name)
            apps[name] = (frozen / name).read_bytes()
            (work / name).write_bytes(apps[name])
        report['frozen_bex1'] = manifest
        inspector = work / 'inspect-executable'
        command(work, 'build-inspector', [tool('gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', runtime / 'src', ROOT / 'tests/executable_inspect_host.c',
                runtime / 'src/executable.c', '-o', inspector])
        for name in ('memory-a', 'memory-b'):
            app = work / (name + '.bex')
            command(work, 'build-' + name, [sys.executable, runtime / 'tools/build_app.py',
                    ROOT / 'tests/larger_memory_fixture_app.c', app, '--format', 'bex2',
                    '--workspace-bytes', WORKSPACE, '--stack-bytes', 65536,
                    '--required-abi-minor', 1, '--elf-output', work / (name + '.elf')])
            plan = json.loads(command(work, 'plan-' + name, [inspector, app]))
            assert plan['workspace'][1:] == [WORKSPACE, 768]
            assert plan['table_pages'] == 2 and plan['owned_pages'] == plan['mapped_pages'] + 2
            assert plan['owned_pages'] <= 1024
            report['plans'][name] = plan
            apps[app.name] = app.read_bytes()
        source = ROOT / 'tests/larger_memory_guest.c'
        command(work, 'compile-fixture', [tool('gcc'), '-std=gnu11', '-Os', '-g', '-Wall', '-Wextra', '-Werror',
                '-ffreestanding', '-m32', '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                '-mno-sse', '-mno-mmx', '-msoft-float', '-I', runtime, '-I', runtime / 'src',
                '-I', args.build, '-c', source, '-o', work / 'kernel.o'])
        objects = [p for p in sorted(args.build.glob('*.o')) if p.name != 'kernel.o']
        command(work, 'link-fixture', [tool('ld'), '-T', args.build / 'linker.ld', '-nostdlib', '-m', 'elf_i386',
                '-z', 'noexecstack', '-o', work / 'kernel.elf', work / 'kernel.o', *objects])
        command(work, 'binary-fixture', [tool('objcopy'), '-O', 'binary', work / 'kernel.elf', work / 'kernel.bin'])
        report['linked_production_objects'] = {p.name: desktop.digest(p) for p in objects}
        c = modules['layout'].constants()
        boot = bytearray(c['DISK_SECTORS'] * 512)
        boot[:512] = (args.build / 'boot.bin').read_bytes()
        modules['update_image'].install_kernel(boot, (work / 'kernel.bin').read_bytes(), c)
        (work / 'boot.img').write_bytes(boot)
        documents = {'source.bin': expected_files()['/Documents/source.bin']}
        (work / 'source.bin').write_bytes(documents['source.bin'])
        report['original_files'] = {'/' + parent + '/' + name: dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                                   for parent, entries in (('Programs', apps), ('Documents', documents))
                                   for name, data in entries.items()}
        for profile in (('default', 'large') if args.disk_profile == 'both' else (args.disk_profile,)):
            path = work / (profile + '.img')
            desktop.seed_disk(path, profile, apps, documents, modules)
            report['profiles'][profile] = dict(ram_mib=256, initial_disk=desktop.artifact(path))
        report['prepared_files'] = {p.name: desktop.artifact(p) for p in sorted(work.iterdir())
                                    if p.is_file() and p.name != 'report.json'}
        report['status'] = 'prepared; no guest started'
    except BaseException as error:
        report.update(status='preparation failed; no guest started', error=repr(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    return report


def decode_counts(text, plans, reboot=False):
    rows = []
    for match in re.finditer(r'AS-COUNT ([\w-]+)\b' + ''.join(r' ' + f + r'=(\d+)' for f in FIELDS), text):
        rows.append(dict(label=match[1], **dict(zip(FIELDS, map(int, match.groups()[1:])))))
    if [r['label'] for r in rows] != (['baseline', 'reboot'] if reboot else list(CHECKPOINTS)):
        raise AssertionError('Missing, reordered or extra page-accounting checkpoints')
    boot = re.search(r'Owned pages total=(\d+) allocated=(\d+) high_water=(\d+)', text)
    if not boot or tuple(map(int, boot.groups()[1:])) != (0, 0):
        raise AssertionError('Missing clean allocator boot counts')
    total = int(boot[1])
    a, b = plans['memory-a'], plans['memory-b']
    peak = a['owned_pages'] + b['owned_pages'] + 16
    populations = ((0, 0, 0),) * 2 if reboot else (
        (0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (1, 1, 1),
        (1, 0, 1), (1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 1, 1),
        (0, 0, 1), (0, 0, 0), (0, 0, 0))
    high = 0
    for row, (na, nb, legacy) in zip(rows, populations):
        owned = na * a['owned_pages'] + nb * b['owned_pages'] + legacy * 16
        mapped = na * a['mapped_pages'] + nb * b['mapped_pages']
        high = max(high, owned)
        expected = dict(total=total, free=total-owned, allocated=owned, high_water=high,
                        backing=legacy*16, mapped=mapped, pt=na+nb, pd=na+nb, records=na+nb+legacy)
        if any(row[field] != value for field, value in expected.items()):
            raise AssertionError('Actual accounting mismatch at ' + row['label'])
    if not reboot and rows[-1]['high_water'] != peak:
        raise AssertionError('High-water did not include both full workspaces and frozen peer')
    apps = []
    for match in re.finditer(r'LM-APP ([\w-]+)\b' + ''.join(r' ' + f + r'=(\d+)' for f in APP_FIELDS), text):
        apps.append(dict(label=match[1], **dict(zip(APP_FIELDS, map(int, match.groups()[1:])))))
    if not reboot:
        labels = ('a-filled', 'b-filled', 'a-retained', 'b-retained', 'a-after-peer-close',
                  'b-zero-relaunch', 'b-after-peer-close', 'a-zero-relaunch')
        if [row['label'] for row in apps] != list(labels):
            raise AssertionError('Missing complete application SDK reports')
        for row in apps:
            plan = a if row['label'].startswith('a-') else b
            if not (row['workspace'] == WORKSPACE and row['checksum'] == expected_checksum(6 if row['label'].startswith('a-') else 2)
                    and row['tables'] == plan['table_pages'] and row['owned'] == plan['owned_pages']
                    and row['mapped'] == plan['mapped_pages'] and not row['reserved']):
                raise AssertionError('App public-memory/full-workspace checksum mismatch')
            if row['label'].endswith('filled'):
                assert row['stage'] == 1 and row['checks'] == 0
            else:
                assert row['stage'] == 2 and row['checks'] > 0
        assert apps[0]['owner'] != apps[1]['owner']
        assert apps[5]['owner'] != apps[1]['owner'] and apps[7]['owner'] != apps[0]['owner']
    return dict(actual_e820_filtered_total=total, checkpoints=rows, application_reports=apps)


def run(args, profile, report, reboot=False):
    label = profile + ('-reboot' if reboot else '-first')
    log, errors = args.work / (label + '.log'), args.work / (label + '.stderr')
    marker = 'LARGER-MEMORY-REBOOT-PASS' if reboot else 'LARGER-MEMORY-FUNCTIONAL-PASS'
    cmd = ['qemu-system-i386', '-m', '256M', '-vga', 'std', '-boot', 'a',
           '-drive', f'file={args.work / "boot.img"},format=raw,index=0,if=floppy',
           '-drive', f'file={args.work / (profile + ".img")},format=raw,index=0,if=ide,cache=writeback',
           '-serial', f'file:{log}', '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none']
    record = report['profiles'][profile]['reboot' if reboot else 'first'] = dict(command=cmd, passed=False)
    (args.work / (label + '.command.json')).write_text(json.dumps(cmd, indent=2) + '\n')
    log.write_text('')
    started = time.monotonic()
    try:
        with errors.open('w') as stream:
            process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stream)
            try:
                while marker not in log.read_text():
                    text = log.read_text()
                    if 'PANIC:' in text or process.poll() is not None or time.monotonic() - started > args.timeout:
                        raise AssertionError(f'{label}: expected {marker}\n{text}\n{errors.read_text()}')
                    time.sleep(.1)
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
                    raise AssertionError('QEMU did not close normally')
        text = log.read_text()
        if 'PANIC:' in text:
            raise AssertionError(text)
        record.update(decode_counts(text, report['plans'], reboot=reboot))
        latency = re.search(r'AS-LATENCY ticks_hz=(\d+) launch=(\d+) close=(\d+) slice=(\d+)', text)
        if not reboot and not latency:
            raise AssertionError('Missing observed coarse fixture timing')
        if latency:
            record['latency_ticks'] = dict(zip(('hz', 'launch', 'close', 'slice'), map(int, latency.groups())))
        record['passed'] = True
    finally:
        record['seconds'] = round(time.monotonic() - started, 3)
        record['artifacts'] = {p.name: desktop.artifact(p) for p in (log, errors) if p.exists()}
    return record


def validate_stopped(path, report, volume):
    nodes = volume.load(path.read_bytes())[2]
    outputs = {}
    for name, data in expected_files().items():
        actual = nodes[volume.resolve(nodes, name)]['data']
        if actual != data:
            raise AssertionError('Exact native file mismatch: ' + name)
        outputs[name] = dict(bytes=len(actual), sha256=hashlib.sha256(actual).hexdigest())
    for name, expected in report['original_files'].items():
        data = nodes[volume.resolve(nodes, name)]['data']
        if dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) != expected:
            raise AssertionError('Original frozen executable/input changed: ' + name)
    return outputs


def main(args):
    if args.memory_mib != 256:
        raise ValueError('Only the explicitly selected optional 256 MiB gate belongs here')
    args.build, args.work = args.build.resolve(), args.work.resolve()
    if not args.prepare_only and not args.qemu_slot_held:
        raise ValueError('Execution requires explicit --qemu-slot-held after coordinator approval')
    runtime, modules = desktop.runtime_tools(args.build)
    if args.prepare_only:
        prepare(args, runtime, modules)
        print('Prepared only; no guest started. ' + str(args.work / 'report.json'))
        return
    path = args.work / 'report.json'
    report = json.loads(path.read_text())
    try:
        if report['status'] != 'prepared; no guest started':
            raise ValueError('Never reuse started/failed evidence; prepare a fresh directory')
        requested_profiles = {'default', 'large'} if args.disk_profile == 'both' else {args.disk_profile}
        if set(report['profiles']) != requested_profiles:
            raise ValueError('Requested disk profiles differ from prepared profile set')
        if report.get('optional_memory_mib') != 256 or any(p['ram_mib'] != 256 for p in report['profiles'].values()):
            raise ValueError('Prepared optional memory selection differs from explicit 256 MiB')
        if report['fixture_sources'] != source_identity():
            raise ValueError('Fixture sources changed after preparation')
        for name, expected in report['prepared_files'].items():
            if desktop.artifact(args.work / name) != expected:
                raise ValueError('Prepared bytes changed: ' + name)
        current = desktop.source_provenance(args, runtime, modules)
        for field in PROVENANCE_FIELDS:
            if current[field] != report['provenance'][field]:
                raise ValueError('Runtime identity changed after preparation: ' + field)
    except BaseException as error:
        # Preserve the admitted/prepared evidence unchanged on a rejected retry.
        refusal = args.work / ('admission-refusal-' + str(time.time_ns()) + '.json')
        with refusal.open('x') as stream:
            json.dump(dict(passed=False, guest_started=False, error=repr(error),
                           report_before=desktop.artifact(path)), stream, indent=2)
            stream.write('\n')
        raise
    try:
        report.update(status='running', execution_provenance=current)
        path.write_text(json.dumps(report, indent=2) + '\n')
        for profile, item in report['profiles'].items():
            disk = args.work / (profile + '.img')
            run(args, profile, report)
            item['before_reboot_files'] = validate_stopped(disk, report, modules['volume'])
            item['before_reboot_disk'] = desktop.artifact(disk)
            run(args, profile, report, reboot=True)
            item['after_reboot_files'] = validate_stopped(disk, report, modules['volume'])
            assert item['before_reboot_files'] == item['after_reboot_files']
            assert item['first']['actual_e820_filtered_total'] == item['reboot']['actual_e820_filtered_total']
            item['final_disk'] = desktop.artifact(disk)
            item['passed'] = True
            path.write_text(json.dumps(report, indent=2) + '\n')
        report.update(passed=True, status='passed')
    except BaseException as error:
        report.update(passed=False, status='failed', error=repr(error))
        raise
    finally:
        report['evidence_artifacts'] = {p.name: desktop.artifact(p) for p in sorted(args.work.iterdir())
                                        if p.is_file() and p.name != 'report.json'}
        path.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: optional 256 MiB ordinary 3 MiB coexistence, exact cleanup and reboot files.')
    print(path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--expected-revision', required=True)
    parser.add_argument('--build-log', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--memory-mib', type=int, choices=(256,), required=True)
    parser.add_argument('--disk-profile', choices=('default', 'large', 'both'), default='both')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-only', action='store_true')
    mode.add_argument('--run-prepared', action='store_true')
    parser.add_argument('--qemu-slot-held', action='store_true')
    parser.add_argument('--timeout', type=int, default=240)
    main(parser.parse_args())
