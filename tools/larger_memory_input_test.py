"""Opt-in 256 MiB/two 3 MiB production PS/2 coexistence and reboot gate.

Prepare-only builds valid SDK clients and disposable existing-format media; it
cannot start QEMU. Run-prepared requires explicit coordinator slot ownership.
Only PlatformSession PS/2, screenshots, serial and stopped-disk decoding are used.
This does not alter or replace the existing 64/default and 128/large gates.
"""
import argparse
from functools import lru_cache
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image
import address_space_input_test as desktop

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = 3 * 1024 * 1024
SDK_FIELDS = ('mapped_pages', 'owned_pages', 'table_pages', 'policy_pages',
              'pool_total_pages', 'pool_free_pages', 'region_count')
SDK_LABELS = ('Mapped pages: ', 'Owned pages: ', 'Table pages: ', 'Policy pages: ',
              'Pool total: ', 'Pool free: ', 'Regions: ')
PREPARED = 'prepared; no guest started'


@lru_cache(maxsize=2)
def checksum(variant):
    if variant not in (1, 2):
        raise ValueError('Expected one of two valid variants')
    value = 2166136261
    for offset in range(WORKSPACE):
        page, within = divmod(offset, 4096)
        if within < 4:
            tag = page ^ ((variant * 2654435769) & 0xffffffff)
            byte = tag.to_bytes(4, 'little')[within]
        else:
            byte = (offset * 37 + page * 17 + variant * 53) % 256
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def expected_report(client, counts):
    fields = [('Variant: ', client['variant']), ('Generation: ', client['generation']),
              ('Task: ', client['task']), ('Round: ', client['round']),
              ('Workspace bytes: ', WORKSPACE), ('Zero bytes: ', WORKSPACE),
              ('Checksum: ', checksum(client['variant']))]
    fields.extend((label, counts[key]) for key, label in zip(SDK_FIELDS, SDK_LABELS))
    return ('BEX2 larger memory\n' + ''.join(f'{label}{value}\n' for label, value in fields)).encode()


def identity(client):
    return f"variant {client['variant']} generation {client['generation']} task {client['task']}"


def report_path(client):
    return (f"/Documents/mem-v{client['variant']}-g{client['generation']}-"
            f"t{client['task']}-r{client['round']}.txt")


def source_identity():
    return {str(path.relative_to(ROOT)): desktop.artifact(path) for path in
            (Path(__file__), ROOT / 'tests/larger_memory_app.c', ROOT / 'tests/larger_memory_app_host.c',
             ROOT / 'tests/larger_memory_app_shim.h', ROOT / 'tests/test_larger_memory_input.py')}


def executable_plan(data):
    h = struct.unpack_from('<16I', data)
    if h[:4] != (0x32584542, 64, 1, 0) or h[10:14] != (WORKSPACE, 65536, 1, 1):
        raise ValueError('Client is not the requested ordinary 3 MiB BEX2')
    mapped = (h[6] + 4095) // 4096 + (h[9] + 4095) // 4096 + h[10] // 4096 + h[11] // 4096
    return dict(workspace_bytes=h[10], stack_bytes=h[11], mapped_pages=mapped,
                table_pages=2, owned_pages=mapped + 2, text_bytes=h[6], data_bytes=h[9])


def write_report(path, report):
    path.write_text(json.dumps(report, indent=2) + '\n')


def prepare(args, runtime, modules):
    work = args.work
    if work.exists() and any(work.iterdir()):
        raise ValueError('Preparation requires a fresh empty --work directory')
    work.mkdir(parents=True, exist_ok=True)
    path = work / 'report.json'
    report = dict(passed=False, status='preparing', profiles={}, plans={},
                  evidence_class='unchanged production PS/2; optional 256 MiB, two 3 MiB workspaces',
                  limits='No guest acceptance until run-prepared passes; pool counts come from SDK snapshots.')
    write_report(path, report)
    try:
        if args.memory_mib != 256:
            raise ValueError('This separate optional gate requires --memory-mib 256')
        report['provenance'] = desktop.source_provenance(args, runtime, modules)
        report['gate_sources'] = source_identity()
        frozen = ROOT / 'tests/fixtures/bex1-legacy'
        manifest = json.loads((frozen / 'manifest.json').read_text())
        counter = frozen / 'counter.bex'
        if desktop.artifact(counter) != manifest['files']['counter.bex']:
            raise ValueError('Frozen Counter bytes changed')
        apps = {'counter.bex': counter.read_bytes()}
        (work / 'counter.bex').write_bytes(apps['counter.bex'])
        report['frozen_counter'] = dict(manifest=desktop.artifact(frozen / 'manifest.json'),
                                        artifact=desktop.artifact(counter))
        for variant, name in ((1, 'larger-a'), (2, 'larger-b')):
            wrapper = work / (name + '.c')
            wrapper.write_text(f'#define LARGER_MEMORY_VARIANT {variant}\n#include ' +
                               json.dumps(str(ROOT / 'tests/larger_memory_app.c')) + '\n')
            output = work / (name + '.bex')
            command = [sys.executable, str(runtime / 'tools/build_app.py'), str(wrapper), str(output),
                       '--format', 'bex2', '--workspace-bytes', str(WORKSPACE), '--stack-bytes', '65536',
                       '--required-abi-minor', '1', '--elf-output', str(output.with_suffix('.elf'))]
            built = subprocess.run(command, capture_output=True, text=True)
            (work / (name + '.log')).write_text(built.stdout + built.stderr)
            built.check_returncode()
            apps[output.name] = output.read_bytes()
            report['plans'][str(variant)] = dict(executable_plan(apps[output.name]), variant=variant,
                name=output.name, command=command, artifact=desktop.artifact(output), checksum=checksum(variant))
            write_report(path, report)
        if apps['larger-a.bex'] == apps['larger-b.bex'] or checksum(1) == checksum(2):
            raise ValueError('The two clients must have distinct binaries and full-byte checksums')
        documents = {'relaunch.txt': b'Ordinary immutable argument selects the fresh-zero generation.\n'}
        report['original_files'] = {}
        for parent, entries in (('Programs', apps), ('Documents', documents)):
            for name, data in entries.items():
                local = work / name
                if not local.exists():
                    local.write_bytes(data)
                report['original_files']['/' + parent + '/' + name] = desktop.artifact(local)
        constants = modules['layout'].constants()
        boot = bytearray(constants['DISK_SECTORS'] * 512)
        boot[:512] = (args.build / 'boot.bin').read_bytes()
        modules['update_image'].install_kernel(boot, (args.build / 'kernel.bin').read_bytes(), constants)
        (work / 'boot.img').write_bytes(boot)
        profiles = ('default', 'large') if args.profile == 'both' else (args.profile,)
        for profile in profiles:
            disk = work / (profile + '.img')
            desktop.seed_disk(disk, profile, apps, documents, modules)
            report['profiles'][profile] = dict(ram_mib=256, disk_format=profile,
                                               initial_disk=desktop.artifact(disk))
        report['prepared_files'] = {p.name: desktop.artifact(p) for p in sorted(work.iterdir())
                                    if p.is_file() and p != path}
        report['status'] = PREPARED
    except BaseException as error:
        report.update(status='preparation failed', error=repr(error))
        raise
    finally:
        write_report(path, report)
    return report


def ready(session, font, variant, generation, label, timeout):
    choices = [f'Held variant {variant} generation {generation} task {task}' for task in range(1, 9)]
    observed = desktop.wait_visible(session, font, [f'Zero bytes: {WORKSPACE}'], label, timeout, choices)
    return dict(variant=variant, generation=generation,
                task=int(observed['alternative'].split()[-1]), round=0)


def read_decimal(font, pixels, prefix):
    """Read only exact production glyphs following a fully matched prefix."""
    mask = font.screenshot_mask(pixels)
    results = set()
    for x, y in font.find_mask(mask, prefix):
        cursor, digits = x + len(prefix) * 8, ''
        for _ in range(10):
            tile = mask[y:y + 18, cursor:cursor + 8]
            matched = [str(n) for n in range(10) if np.array_equal(tile, font.template(str(n)))]
            if not matched:
                break
            digits += matched[0]
            cursor += 8
        if digits and np.array_equal(mask[y:y + 18, cursor:cursor + 8], font.template(' ')) and font.find_mask(mask, prefix + digits):
            results.add(int(digits))
    if len(results) != 1:
        raise AssertionError('No unique exact visible SDK number for ' + prefix)
    return results.pop()


def focus_client(session, font, target, clients, item, timeout):
    # A never-reused numeric challenge is acknowledged through normal input by
    # the target. Neither retained old text nor a background window is enough.
    for _ in range(9):
        item['focus_token'] = item.get('focus_token', 0) + 1
        token = item['focus_token']
        session.text(str(token));session.key('ret')
        choices = [f'Ack {identity(c)} token {token}' for c in clients]
        try:
            result = desktop.wait_visible(session, font, [], 'focus-' + str(token), min(timeout, 2), choices)
        except AssertionError as error:
            session.events.append(dict(kind='focus-search-miss', label='focus-' + str(token), error=repr(error)))
            if session.process.poll() is not None or 'PANIC:' in session.log.read_text():
                raise
        else:
            if result['alternative'] == f'Ack {identity(target)} token {token}':
                return
        session.key('ctrl-tab', delay=.5)
    raise AssertionError('Could not acknowledge foreground ' + identity(target))


def require_adjacent_durable_path(visible, path):
    # Separate ordinary PRINT calls occupy separate production Terminal lines.
    # Keep the exact owner/generation/round path immediately below its label;
    # matching unrelated text elsewhere in the screenshot is insufficient.
    labels = visible['lines'].get('Durable', [])
    paths = visible['lines'].get(path, [])
    if not any(px == x and py == y + 19 for x, y in labels for px, py in paths):
        raise AssertionError('Durable label and exact owner report path are not adjacent')


def verify_client(session, font, client, plan, item, saved, timeout, label):
    client['round'] += 1
    session.key('v')
    path = report_path(client)
    fixed = ['BEX2 larger memory', f"Variant: {client['variant']}",
             f"Generation: {client['generation']}", f"Task: {client['task']}", f"Round: {client['round']}",
             f'Workspace bytes: {WORKSPACE}', f'Zero bytes: {WORKSPACE}',
             f"Checksum: {plan['checksum']}", f"Mapped pages: {plan['mapped_pages']}",
             f"Owned pages: {plan['owned_pages']}", 'Table pages: 2', 'Durable', path]
    visible = desktop.wait_visible(session, font, fixed, label, timeout)
    require_adjacent_durable_path(visible, path)
    pixels = np.asarray(Image.open(session.directory / (label + '.png')).convert('RGB'))
    counts = {key: read_decimal(font, pixels, prefix) for key, prefix in zip(SDK_FIELDS, SDK_LABELS)}
    check_memory_counts(counts, plan, item['serial_boot_baseline'], item['live_owned_pages'])
    expected = expected_report(client, counts)
    if not all(font.find(pixels, line) for line in expected.decode().splitlines()):
        raise AssertionError('Complete expected report is not visible')
    if path in saved:
        raise AssertionError('Fresh report path was reused')
    saved[path] = expected
    item.setdefault('sdk_snapshots', []).append(dict(client=dict(client), counts=counts,
        live_clients=[dict(c) for c in item['live_bex2']], expected_live_owned_pages=item['live_owned_pages'],
        report=path, checksum=plan['checksum'], screenshot=str(session.directory / (label + '.png'))))


def record_live_clients(item, clients, plans, reason):
    item['live_bex2'] = [dict(c) for c in clients]
    # Frozen BEX1 Counter owns its unchanged sixteen 4 KiB backing pages.
    item['live_owned_pages'] = 16 + sum(plans[str(c['variant'])]['owned_pages'] for c in clients)
    item.setdefault('ownership_events', []).append(dict(reason=reason,
        clients=[dict(c) for c in clients], expected_owned_pages=item['live_owned_pages']))


def check_memory_counts(counts, plan, baseline, live_owned_pages):
    for field in ('mapped_pages', 'owned_pages', 'table_pages'):
        if counts[field] != plan[field]:
            raise AssertionError('SDK count differs from parsed executable: ' + field)
    if not (counts['policy_pages'] == 1024 and counts['owned_pages'] <= counts['policy_pages'] and
            1 <= counts['region_count'] <= 4):
        raise AssertionError('SDK memory snapshot is inconsistent')
    if counts['pool_total_pages'] != baseline['total_pages']:
        raise AssertionError('SDK total differs from actual production serial baseline')
    if counts['pool_free_pages'] != baseline['total_pages'] - live_owned_pages:
        raise AssertionError('SDK free pages differ from live BEX2 footprints plus frozen Counter')


def focus_counter(session, decode, item, timeout):
    for _ in range(9):
        item['counter_focus_token'] = item.get('counter_focus_token', 0) + 1
        label = 'counter-focus-' + str(item['counter_focus_token'])
        try:
            return desktop.wait_counter(session, decode, lambda value: True, label, min(timeout, 2))
        except AssertionError as error:
            record = dict(kind='counter-focus-search-miss', label=label, error=repr(error))
            session.events.append(record)
            try:
                session.frame(label + '-timeout')
            except Exception as capture_error:
                record['capture_error'] = repr(capture_error)
            if session.process.poll() is not None or 'PANIC:' in session.log.read_text():
                raise
        session.key('ctrl-tab', delay=.5)
    raise AssertionError('Frozen Counter could not be focused')


def serial_baseline(serial):
    matches = re.findall(r'Owned pages total=(\d+) allocated=(\d+) high_water=(\d+)', serial)
    if len(matches) != 1:
        raise AssertionError('Expected one actual production allocator boot baseline')
    total, allocated, high_water = map(int, matches[0])
    if allocated or high_water or not total:
        raise AssertionError('Expected initially empty owned-page pool')
    return dict(total_pages=total, allocated_pages=allocated, high_water_pages=high_water)


def run_profile(args, profile, report, modules, font):
    item = report['profiles'][profile]
    disk = args.work / (profile + '.img')
    extra = ['-m', '256M', '-drive', f'file={disk},format=raw,index=0,if=ide,cache=writeback', '-nic', 'none']
    Session = modules['platform_evidence'].PlatformSession
    decode = modules['platform_foundation_test'].counter_canvas
    saved = {};item['sessions'] = []
    with Session(args.build, 'larger-memory-' + profile, extra=extra, image=args.work / 'boot.img') as session:
        item['sessions'].append(str(session.directory));item.setdefault('qemu_commands', {})[str(session.directory)] = session.process.args
        session.boot()
        item['serial_boot_baseline'] = serial_baseline(session.log.read_text())
        session.launch('counter.bex')
        before = desktop.wait_counter(session, decode, lambda v: not v['paused'], 'counter-initial', args.timeout)
        item['counter_initial'] = before
        clients = []
        for variant in (1, 2):
            session.launch(report['plans'][str(variant)]['name']);session.key('alt-ret')
            clients.append(ready(session, font, variant, 1, 'filled-' + str(variant), args.timeout))
            record_live_clients(item, clients, report['plans'], 'launch-' + str(variant))
        first, second = clients
        if first['task'] == second['task']:
            raise AssertionError('Two owners did not coexist')
        item['initial_clients'] = [dict(c) for c in clients]
        # Both complete writes are observed before either verification starts.
        for client in (second, first, second, first):
            focus_client(session, font, client, clients, item, args.timeout)
            verify_client(session, font, client, report['plans'][str(client['variant'])], item,
                          saved, args.timeout, f"interleave-v{client['variant']}-r{client['round'] + 1}")
        focus_counter(session, decode, item, args.timeout)
        advanced = desktop.wait_counter(session, decode, lambda v: not v['paused'] and v['value'] > before['value'],
                                        'counter-advanced-with-two-owners', args.timeout)
        session.key('spc')
        paused = desktop.wait_counter(session, decode, lambda v: v['paused'], 'counter-paused-with-two-owners', args.timeout)
        if paused['value'] >= 9990:
            raise AssertionError('Counter exceeded non-wrapping exact-file range')
        session.key('equal')
        acknowledged = desktop.wait_counter(session, decode, lambda v: v['paused'] and v['value'] == paused['value'] + 10,
                                             'counter-input-with-two-owners', args.timeout)
        modules['platform_foundation_test'].quiet(session, args.timeout);session.key('s')
        visible = desktop.wait_visible(session, font, [], 'counter-save-with-two-owners', args.timeout,
                                       [f'Saved /Documents/counter-{i}.txt' for i in range(1, 9)])
        saved[visible['alternative'][6:]] = (str(paused['value'] + 10) + '\n').encode()
        item['counter_during_coexistence'] = dict(advanced=advanced, paused=paused, acknowledged=acknowledged)
        for client in clients:
            focus_client(session, font, client, clients, item, args.timeout)
            verify_client(session, font, client, report['plans'][str(client['variant'])], item,
                          saved, args.timeout, 'after-counter-v' + str(client['variant']))
        # Close a held owner through normal desktop input, then explicitly
        # challenge the remaining owner before examining every byte again.
        focus_client(session, font, second, clients, item, args.timeout);session.key('ctrl-w')
        clients.remove(second)
        record_live_clients(item, clients, report['plans'], 'close-second-owner')
        focus_client(session, font, first, clients, item, args.timeout)
        verify_client(session, font, first, report['plans']['1'], item, saved, args.timeout, 'survivor-after-close')
        session.key('ctrl-n');session.key('alt-ret')
        session.text('start /Programs/larger-b.bex /Documents/relaunch.txt');session.key('ret')
        replacement = ready(session, font, 2, 2, 'replacement-fresh-zero', args.timeout)
        if replacement['task'] == first['task']:
            raise AssertionError('Relaunch replaced the survivor')
        clients.append(replacement);item['replacement'] = dict(replacement)
        record_live_clients(item, clients, report['plans'], 'same-app-fresh-zero-relaunch')
        for client in (replacement, first, replacement):
            focus_client(session, font, client, clients, item, args.timeout)
            verify_client(session, font, client, report['plans'][str(client['variant'])], item,
                          saved, args.timeout, f"replacement-era-v{client['variant']}-r{client['round'] + 1}")
        for client in (replacement, first):
            focus_client(session, font, client, clients, item, args.timeout)
            session.key('q');desktop.wait_visible(session, font, ['Native task finished.'],
                                                'normal-exit-v' + str(client['variant']), args.timeout)
            session.key('ctrl-w');clients.remove(client)
            record_live_clients(item, clients, report['plans'], 'normal-exit-' + str(client['variant']))
        focus_counter(session, decode, item, args.timeout);session.key('q')
        desktop.wait_visible(session, font, ['Native task finished.'], 'counter-normal-exit', args.timeout)
        session.key('ctrl-w');desktop.shutdown(session, args.timeout)
    item['before_reboot_files'] = desktop.validate_stopped_files(disk, report, saved, modules['volume'])
    item['reboot_input_disk'] = desktop.artifact(disk)
    # No reseed or kernel reinstall: the exact stopped data image is reused.
    with Session(args.build, 'larger-memory-' + profile + '-reboot', extra=extra, image=args.work / 'boot.img') as session:
        item['sessions'].append(str(session.directory));item['qemu_commands'][str(session.directory)] = session.process.args
        session.boot()
        item['serial_reboot_baseline'] = serial_baseline(session.log.read_text())
        if item['serial_reboot_baseline'] != item['serial_boot_baseline']:
            raise AssertionError('Actual owned-page boot baseline changed across reboot')
        session.launch('terminal');session.key('alt-ret')
        for index, (path, contents) in enumerate(saved.items()):
            session.text('clear');session.key('ret');session.text('cat ' + path);session.key('ret')
            desktop.wait_visible(session, font, ['cat ' + path] + contents.decode().splitlines(),
                                 'reboot-report-' + str(index), args.timeout)
        session.key('ctrl-w');desktop.shutdown(session, args.timeout)
    item['after_reboot_files'] = desktop.validate_stopped_files(disk, report, saved, modules['volume'])
    if item['after_reboot_files'] != item['before_reboot_files']:
        raise AssertionError('Exact native files changed across normal reboot')
    item['final_disk'] = desktop.artifact(disk);item['passed'] = True


def main(args):
    args.build = args.build.resolve();args.work = args.work.resolve()
    if args.memory_mib != 256:
        raise ValueError('Use explicit --memory-mib 256 for this optional gate')
    if not args.prepare_only and not args.qemu_slot_held:
        raise ValueError('Execution requires explicit --qemu-slot-held after coordinator approval')
    runtime, modules = desktop.runtime_tools(args.build)
    if args.prepare_only:
        prepare(args, runtime, modules)
        print('Prepared only; no QEMU. ' + str(args.work / 'report.json'));return
    path = args.work / 'report.json'
    report = json.loads(path.read_text())
    try:
        if report['status'] != PREPARED:
            raise ValueError('Never reuse a started evidence directory; prepare fresh media')
        requested_profiles = {'default', 'large'} if args.profile == 'both' else {args.profile}
        if set(report['profiles']) != requested_profiles:
            raise ValueError('Requested disk-profile set differs from prepared profiles')
        if source_identity() != report['gate_sources']:
            raise ValueError('Gate sources changed after preparation')
        for name, expected in report['prepared_files'].items():
            if desktop.artifact(args.work / name) != expected:
                raise ValueError('Prepared bytes changed: ' + name)
        if any(item['ram_mib'] != 256 for item in report['profiles'].values()):
            raise ValueError('Prepared RAM profile is not the explicit optional mode')
        current = desktop.source_provenance(args, runtime, modules)
        for field in ('built_source', 'artifacts', 'runtime_source_sha256', 'runtime_helper_sha256',
                      'production_object_sha256', 'harness_sha256'):
            if current[field] != report['provenance'][field]:
                raise ValueError('Runtime provenance changed: ' + field)
    except BaseException as error:
        # Admission refusal must not rewrite a prepared or previously accepted report.
        index = 1
        while (args.work / f'admission-refused-{index:03d}.json').exists():
            index += 1
        write_report(args.work / f'admission-refused-{index:03d}.json',
                     dict(status='admission refused; no guest started', error=repr(error)))
        raise
    report['execution_provenance'] = current;report['status'] = 'running';write_report(path, report)
    try:
        font = desktop.VisibleText(runtime / 'src/font.h')
        for profile in report['profiles']:
            run_profile(args, profile, report, modules, font);write_report(path, report)
        report.update(passed=True, status='passed')
    except BaseException as error:
        report.update(passed=False, status='failed', error=repr(error));raise
    finally:
        report['evidence_artifacts'] = {str(p): desktop.artifact(p) for item in report['profiles'].values()
            for directory in item.get('sessions', []) for p in sorted(Path(directory).iterdir()) if p.is_file()}
        write_report(path, report)
    print('PASS: two 3 MiB workspaces, frozen Counter, zero relaunch, survivor checks and exact reboot files.')
    print(path)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path);parser.add_argument('--expected-revision', required=True)
    parser.add_argument('--build-log', type=Path, required=True);parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--memory-mib', type=int, choices=(256,), required=True)
    parser.add_argument('--disk-profile', '--profile', dest='profile', choices=('default', 'large', 'both'), default='both')
    parser.add_argument('--timeout', type=int, default=180)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-only', action='store_true');mode.add_argument('--run-prepared', action='store_true')
    parser.add_argument('--qemu-slot-held', action='store_true')
    return parser.parse_args(argv)


if __name__ == '__main__':
    main(parse_args())
