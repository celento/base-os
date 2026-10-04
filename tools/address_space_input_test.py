"""Production BEX2 desktop gates using only normal PS/2 and visible output.

First use --prepare-only, which never starts QEMU. After the coordinator grants
exclusive QEMU ownership, use --run-prepared --qemu-slot-held on that work tree.
The enabled clean production kernel is never rebuilt, relinked, or modified.
No debugger, monitor commands, guest-memory observation, fault injection, socket,
or malformed executable is used. Data-image inspection occurs only while stopped.
"""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = 1 << 20
NOTE = b'This note was saved by a protected C application.\n'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def artifact(path):
    path = Path(path)
    return dict(bytes=path.stat().st_size, sha256=digest(path))


def expected_report(kind, source, workspace=WORKSPACE):
    """Independent unsigned-arithmetic oracle for the unmodified SDK examples."""
    count = workspace // 4
    if kind == 'array':
        seed = 0
        for byte in source:
            seed = (seed * 33 + byte) & 0xffffffff
        checksum = sum(((i * 1664525 + 1013904223) & 0xffffffff) ^ seed
                       for i in range(count)) & 0xffffffff
        return (f'BEX2 workspace array\nInput bytes: {len(source)}\nWords: {count}\n'
                f'Seed: {seed}\nChecksum: {checksum}\n').encode()
    if kind != 'index':
        raise ValueError(kind)
    offsets = [i for i in range(len(source)) if i == 0 or source[i - 1] == 10]
    if len(offsets) > count:
        raise ValueError('Ordinary input exceeds the declared line-index capacity')
    return (f'BEX2 workspace line index\nInput bytes: {len(source)}\n'
            f'Index capacity: {count}\nLines: {len(offsets)}\n'
            f'Offset checksum: {sum(offsets) & 0xffffffff}\n').encode()


class VisibleText:
    """Exact thresholded production monospace glyphs, not probabilistic OCR.

    Only screenshot RGB pixels are observed. At the normal Terminal foreground
    and background, alpha levels 0..7 are below brightness 130 and 8..15 above.
    All 18x8 pixels (including spaces) must match. Origins come from the visible
    screenshot, never window structs, debug symbols, or guest memory.
    """
    def __init__(self, font_path):
        source = Path(font_path).read_text()
        match = re.search(r'edit_font_px\[95\]\[72\]\s*=\s*\{(.*?)\n\};', source, re.S)
        if not match:
            raise ValueError('Production monospace font shape changed')
        values = np.array([int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', match[1])], dtype=np.uint8)
        if values.size != 95 * 72:
            raise ValueError('Production monospace font byte count changed')
        packed = values.reshape(95, 18, 4)
        self.glyphs = np.stack((packed >> 4, packed & 15), axis=-1).reshape(95, 18, 8) >= 8

    def template(self, text):
        if not text or any(not 32 <= ord(char) <= 126 for char in text):
            raise ValueError('Expected one nonempty printable ASCII line')
        return np.concatenate([self.glyphs[ord(char) - 32] for char in text], axis=1)

    @staticmethod
    def screenshot_mask(pixels):
        return np.asarray(pixels, dtype=np.uint16).sum(axis=2) >= 390

    def find(self, pixels, text):
        return self.find_mask(self.screenshot_mask(pixels), text)

    def find_mask(self, mask, text):
        template = self.template(text)
        height, width = mask.shape
        tw = template.shape[1]
        if height < 18 or width < tw:
            return []
        # Locate the first non-space glyph via all of its row bit signatures.
        first = next((i for i, char in enumerate(text) if char != ' '), None)
        if first is None:
            raise ValueError('An all-space line is not visible evidence')
        x0 = first * 8
        rows = np.zeros((height, width - 7), dtype=np.uint8)
        for bit in range(8):
            rows |= mask[:, bit:width - 7 + bit].astype(np.uint8) << (7 - bit)
        expected = np.packbits(template[:, x0:x0 + 8], axis=1)[:, 0]
        candidates = np.ones((height - 17, width - tw + 1), dtype=bool)
        for row in range(18):
            candidates &= rows[row:row + height - 17, x0:x0 + width - tw + 1] == expected[row]
        found = []
        for y, x in zip(*np.where(candidates)):
            if np.array_equal(mask[y:y + 18, x:x + tw], template):
                found.append([int(x), int(y)])
        return found


def runtime_tools(build):
    runtime = Path(build).resolve().parent
    folder = runtime / 'tools'
    sys.path.insert(0, str(folder))
    modules = {name: importlib.import_module(name) for name in
               ('platform_evidence', 'platform_foundation_test', 'volume', 'layout',
                'update_image', 'init_data', 'make_stats_fixture')}
    # Import caching must not silently substitute another checkout's SDK/layout.
    for name in (*modules, 'qemu_session', 'kernel_pack', 'build_app'):
        module = sys.modules[name]
        if Path(module.__file__).resolve().parent != folder:
            raise ValueError('Runtime helper was imported from another checkout: ' + name)
    return runtime, modules


def source_provenance(args, runtime, modules):
    identity = modules['platform_evidence'].provenance(args.build)
    if identity['built_source']['revision'] != args.expected_revision:
        raise ValueError('Built source does not match --expected-revision')
    if not re.search(r'^#define BASEOS_BEX2_ENABLED\s+1\s*$', (runtime / 'src/program.h').read_text(), re.M):
        raise ValueError('Requires tracked source-enabled production runtime')
    log = args.build_log.read_text()
    if not any(' -c src/process.c ' in line for line in log.splitlines()):
        raise ValueError('Missing process compilation in supplied build log')
    if re.search(r'-D\s*BASEOS_BEX2_ENABLED', log):
        raise ValueError('Build-log macro override is not clean source enablement')
    helpers = {}
    for name in ('platform_evidence', 'platform_foundation_test', 'qemu_session', 'kernel_pack',
                 'volume', 'layout', 'update_image', 'init_data', 'make_stats_fixture', 'build_app'):
        relative = 'tools/' + name + '.py'
        expected = subprocess.check_output(['git', '-C', str(runtime), 'show', args.expected_revision + ':' + relative])
        if (runtime / relative).read_bytes() != expected:
            raise ValueError('Runtime helper differs from built revision: ' + relative)
        helpers[relative] = digest(runtime / relative)
    identity.update(runtime_helper_sha256=helpers,
                    runtime_build_log=dict(path=str(args.build_log.resolve()), **artifact(args.build_log)),
                    production_object_sha256={p.name: digest(p) for p in sorted(args.build.glob('*.o'))},
                    harness_revision=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                    harness_status=subprocess.check_output(['git', '-C', str(ROOT), 'status', '--short'], text=True),
                    harness_sha256={str(path.relative_to(ROOT)): digest(path) for path in
                                    (Path(__file__), ROOT / 'tests/test_address_space_input.py')})
    return identity


def seed_disk(path, profile, apps, documents, modules):
    if path.exists():
        raise ValueError('Refusing to overwrite a disk: ' + str(path))
    modules['init_data'].initialize(path, profile=profile)
    volume = modules['volume']
    nodes = {0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
             1: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0),
             2: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0)}
    for parent, entries in ((1, apps), (2, documents)):
        for name, data in entries.items():
            nodes[len(nodes)] = dict(parent=parent, name=name, directory=0, app=0, data=data, modified=1234)
    layout = volume.data_layout(profile)
    header, payload = volume.encode_snapshot(nodes, layout, 1)
    raw = bytearray(path.read_bytes())
    offset = layout.lbas[0] * 512
    raw[offset:offset + 512] = header.ljust(512, b'\0')
    raw[offset + 512:offset + 512 + len(payload)] = payload
    if volume.load(raw)[2] != nodes:
        raise ValueError('Seed snapshot did not round-trip exactly')
    path.write_bytes(raw)
    return nodes


def prepare(args, runtime, modules):
    work = args.work
    if work.exists() and any(work.iterdir()):
        raise ValueError('Preparation requires a fresh empty --work directory')
    work.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, status='prepared; no guest started', profiles={},
                  evidence_class='unchanged clean enabled production kernel; ordinary PS/2 desktop',
                  provenance=source_provenance(args, runtime, modules), build_commands=[])
    frozen_dir = ROOT / 'tests/fixtures/bex1-hour05'
    manifest = json.loads((frozen_dir / 'manifest.json').read_text())
    apps = {}
    for name, info in manifest['files'].items():
        if artifact(frozen_dir / name) != info:
            raise ValueError('Frozen BEX1 artifact differs: ' + name)
        apps[name] = (frozen_dir / name).read_bytes()
        (work / name).write_bytes(apps[name])
    report['frozen_bex1'] = manifest
    report['frozen_manifest_sha256'] = digest(frozen_dir / 'manifest.json')
    for kind in ('array', 'index'):
        output = work / ('workspace-' + kind + '.bex')
        command = [sys.executable, str(runtime / 'tools/build_app.py'),
                   str(runtime / ('examples/c/workspace_' + kind + '.c')), str(output),
                   '--format', 'bex2', '--workspace-bytes', str(WORKSPACE), '--stack-bytes', '65536',
                   '--required-abi-minor', '1', '--elf-output', str(output.with_suffix('.elf'))]
        built = subprocess.run(command, check=True, capture_output=True, text=True)
        (work / ('build-' + kind + '.log')).write_text(built.stdout + built.stderr)
        report['build_commands'].append(command)
        header = struct.unpack_from('<16I', output.read_bytes())
        if header[0] != 0x32584542 or header[10:14] != (WORKSPACE, 65536, 1, 1):
            raise ValueError('SDK output does not declare the requested BEX2 workspace/stack/ABI')
        apps[output.name] = output.read_bytes()
    fallback = modules['make_stats_fixture'].document()
    explicit = fallback + b'\nAn explicit document argument chooses this extra line.\nFinal line.'
    documents = {'stats-sample.txt': fallback, 'index-source.txt': explicit}
    report['expected_reports'] = {'array': expected_report('array', fallback).decode(),
                                  'index': expected_report('index', explicit).decode()}
    report['original_files'] = {}
    for parent, entries in (('Programs', apps), ('Documents', documents)):
        for name, data in entries.items():
            report['original_files']['/' + parent + '/' + name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    for name, data in documents.items():
        (work / name).write_bytes(data)
    constants = modules['layout'].constants()
    boot = bytearray(constants['DISK_SECTORS'] * 512)
    boot[:512] = (args.build / 'boot.bin').read_bytes()
    modules['update_image'].install_kernel(boot, (args.build / 'kernel.bin').read_bytes(), constants)
    (work / 'boot.img').write_bytes(boot)
    for profile in (('default', 'large') if args.profile == 'both' else (args.profile,)):
        path = work / (profile + '.img')
        seed_disk(path, profile, apps, documents, modules)
        report['profiles'][profile] = dict(ram_mib=64 if profile == 'default' else 128, initial_disk=artifact(path))
    # These exact bytes are checked again before admitting any guest session.
    report['prepared_files'] = {p.name: artifact(p) for p in sorted(work.iterdir()) if p.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def wait_visible(session, font, lines, label, timeout, alternatives=()):
    """Require two complete matching screenshots, preserving accepted pixels."""
    deadline = time.monotonic() + timeout
    previous = None
    while time.monotonic() < deadline:
        if session.process.poll() is not None:
            raise AssertionError('Guest exited before ' + label)
        serial = session.log.read_text() if session.log.exists() else ''
        if 'PANIC:' in serial:
            raise AssertionError(serial)
        pixels, wall = session.frame()
        mask = font.screenshot_mask(pixels)
        positions = {line: font.find_mask(mask, line) for line in lines}
        chosen = next((line for line in alternatives if font.find_mask(mask, line)), None)
        if all(positions.values()) and (not alternatives or chosen):
            current = (positions, chosen)
            if current == previous:
                Image.fromarray(pixels).save(session.directory / (label + '.png'))
                record = dict(kind='visible-text', wall=wall, label=label, lines=positions, alternative=chosen)
                session.events.append(record)
                return record
            previous = current
        else:
            previous = None
        if time.monotonic() - session.last_key > 10:
            session.key('shift', 0)
        time.sleep(.08)
    session.frame(label + '-timeout')
    raise AssertionError('Expected visible text not found: ' + label + ': ' + repr(lines))


def wait_counter(session, decode, predicate, label, timeout):
    deadline = time.monotonic() + timeout
    previous = None
    while time.monotonic() < deadline:
        pixels, wall = session.frame()
        value = decode(pixels)
        if value and predicate(value):
            if value == previous:
                Image.fromarray(pixels).save(session.directory / (label + '.png'))
                session.events.append(dict(kind='legacy-counter', label=label, wall=wall, **value))
                return value
            previous = value
        else:
            previous = None
        if session.process.poll() is not None or 'PANIC:' in session.log.read_text():
            raise AssertionError('Guest failed before ' + label)
        time.sleep(.08)
    raise AssertionError('Expected visible legacy Counter not found: ' + label)


def shutdown(session, timeout):
    session.frame('before-normal-shutdown')
    for key in ('f10', 'right', 'right', 'up'):
        session.key(key, delay=.18)
    session.frame('normal-shutdown-menu')
    session.key('ret', delay=0)
    if session.process.wait(timeout=timeout) != 0:
        raise AssertionError('Normal System Shutdown returned a failure')
    if 'PANIC:' in session.log.read_text():
        raise AssertionError(session.log.read_text())
    session.events.append(dict(kind='normal-system-shutdown', returncode=0))


def validate_stopped_files(disk, report, saved, volume):
    nodes = volume.load(disk.read_bytes())[2]
    result = {}
    for path, expected in report['original_files'].items():
        data = nodes[volume.resolve(nodes, path)]['data']
        actual = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        if actual != expected:
            raise AssertionError('Original file changed: ' + path)
        result[path] = actual
    for path, expected in saved.items():
        data = nodes[volume.resolve(nodes, path)]['data']
        if data != expected:
            raise AssertionError('Exact saved report differs: ' + path)
        result[path] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), text=data.decode('ascii'))
    return result


def run_profile(args, profile, report, modules, font):
    item = report['profiles'][profile]
    disk = args.work / (profile + '.img')
    extra = ['-m', str(item['ram_mib']) + 'M', '-drive',
             f'file={disk},format=raw,index=0,if=ide,cache=writeback', '-nic', 'none']
    Session = modules['platform_evidence'].PlatformSession
    decode = modules['platform_foundation_test'].counter_canvas
    saved = {}
    item['sessions'] = []
    with Session(args.build, 'bex2-desktop-' + profile, extra=extra, image=args.work / 'boot.img') as session:
        item['sessions'].append(str(session.directory))
        item.setdefault('qemu_commands', {})[str(session.directory)] = session.process.args
        session.boot()
        session.launch('counter.bex')
        before = wait_counter(session, decode, lambda value: not value['paused'], 'frozen-counter-active', args.timeout)
        item['counter_initial'] = before
        # Launcher makes a distinct Terminal while the frozen BEX1 task remains live.
        session.launch('workspace-array.bex')
        session.key('alt-ret')
        lines = report['expected_reports']['array'].splitlines() + ['Saved and synchronized:', 'Native task finished.']
        visible = wait_visible(session, font, lines, 'array-launcher-result', args.timeout,
                               [f'/Documents/workspace-array-{i}.txt' for i in range(1, 9)])
        saved[visible['alternative']] = report['expected_reports']['array'].encode()
        # The explicit document differs from the fallback, so the checksum proves
        # that production Terminal argument handling selected the intended file.
        session.key('ctrl-n')
        session.key('alt-ret')
        command = 'start /Programs/workspace-index.bex /Documents/index-source.txt'
        session.text(command)
        session.key('ret')
        lines = report['expected_reports']['index'].splitlines() + ['Saved and synchronized:', 'Native task finished.']
        visible = wait_visible(session, font, lines, 'index-terminal-argument-result', args.timeout,
                               [f'/Documents/workspace-index-{i}.txt' for i in range(1, 9)])
        saved[visible['alternative']] = report['expected_reports']['index'].encode()
        session.key('ctrl-w')
        session.key('ctrl-w')
        session.key('alt-ret')
        advanced = wait_counter(session, decode, lambda value: not value['paused'] and value['value'] > before['value'],
                                'frozen-counter-advanced', args.timeout)
        session.key('spc')
        paused = wait_counter(session, decode, lambda value: value['paused'], 'frozen-counter-paused', args.timeout)
        # The frozen canvas shows value % 10000. This short fresh-disk gate
        # refuses the wrapping range rather than treating display digits as the
        # full saved integer. The visual acknowledgment still uses its modulus.
        if paused['value'] >= 9990:
            raise AssertionError('Counter exceeds the non-wrapping exact-report gate')
        session.key('equal')
        after = wait_counter(session, decode, lambda value: value['paused'] and value['value'] == (paused['value'] + 10) % 10000,
                             'frozen-counter-key-ack', args.timeout)
        modules['platform_foundation_test'].quiet(session, args.timeout)
        session.key('s')
        visible = wait_visible(session, font, [], 'frozen-counter-saved', args.timeout,
                               [f'Saved /Documents/counter-{i}.txt' for i in range(1, 9)])
        saved[visible['alternative'][6:]] = (str(paused['value'] + 10) + '\n').encode()
        item['counter_after'] = dict(advanced=advanced, paused=paused, acknowledged=after)
        session.key('q')
        wait_visible(session, font, ['Native task finished.'], 'frozen-counter-normal-exit', args.timeout)
        session.key('ctrl-w')
        modules['platform_foundation_test'].quiet(session, args.timeout)
        session.launch('notebook.bex')
        session.key('alt-ret')
        wait_visible(session, font, ['Saved /Documents/sdk-note.txt', 'Native task finished.'],
                     'frozen-notebook-result', args.timeout)
        saved['/Documents/sdk-note.txt'] = NOTE
        session.key('ctrl-w')
        shutdown(session, args.timeout)
    item['before_reboot_files'] = validate_stopped_files(disk, report, saved, modules['volume'])
    with Session(args.build, 'bex2-desktop-' + profile + '-reboot', extra=extra, image=args.work / 'boot.img') as session:
        item['sessions'].append(str(session.directory))
        item.setdefault('qemu_commands', {})[str(session.directory)] = session.process.args
        session.boot()
        session.launch('terminal')
        session.key('alt-ret')
        for number, (path, contents) in enumerate(saved.items()):
            session.text('clear')
            session.key('ret')
            session.text('cat ' + path)
            session.key('ret')
            wait_visible(session, font, ['cat ' + path] + contents.decode().splitlines(),
                         f'reboot-saved-file-{number}', args.timeout)
        session.key('ctrl-w')
        shutdown(session, args.timeout)
    item['after_reboot_files'] = validate_stopped_files(disk, report, saved, modules['volume'])
    if item['after_reboot_files'] != item['before_reboot_files']:
        raise AssertionError('Reports changed across ordinary reboot')
    item['final_disk'] = artifact(disk)
    item['passed'] = True


def main(args):
    args.build = args.build.resolve()
    args.work = args.work.resolve()
    runtime, modules = runtime_tools(args.build)
    if args.prepare_only:
        prepare(args, runtime, modules)
        print('Prepared only; no guest started. ' + str(args.work / 'report.json'), flush=True)
        return
    if not args.qemu_slot_held:
        raise ValueError('Execution requires explicit --qemu-slot-held after coordinator approval')
    report = json.loads((args.work / 'report.json').read_text())
    if report['status'] != 'prepared; no guest started':
        raise ValueError('Do not rerun a started evidence directory; prepare a fresh fixture')
    for name, expected in report['prepared_files'].items():
        if artifact(args.work / name) != expected:
            raise ValueError('Prepared artifact changed before guest admission: ' + name)
    current = source_provenance(args, runtime, modules)
    for field in ('built_source', 'artifacts', 'runtime_source_sha256', 'runtime_helper_sha256', 'production_object_sha256', 'harness_sha256'):
        if current[field] != report['provenance'][field]:
            raise ValueError('Runtime provenance changed after preparation: ' + field)
    report['execution_provenance'] = current
    report['status'] = 'running'
    report_path = args.work / 'report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    font = VisibleText(runtime / 'src/font.h')
    try:
        for profile in report['profiles']:
            run_profile(args, profile, report, modules, font)
            report_path.write_text(json.dumps(report, indent=2) + '\n')
        report.update(passed=True, status='passed')
    except BaseException as error:
        report.update(passed=False, status='failed', error=repr(error))
        raise
    finally:
        report['evidence_artifacts'] = {}
        for item in report['profiles'].values():
            for directory in item.get('sessions', []):
                for path in sorted(Path(directory).iterdir()):
                    if path.is_file():
                        report['evidence_artifacts'][str(path)] = artifact(path)
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: production BEX2 launcher/Terminal, mixed frozen BEX1, visible results, exact normal-reboot reports.', flush=True)
    print(report_path, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--expected-revision', required=True)
    parser.add_argument('--build-log', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--profile', choices=('default', 'large', 'both'), default='both')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-only', action='store_true')
    mode.add_argument('--run-prepared', action='store_true')
    parser.add_argument('--qemu-slot-held', action='store_true')
    parser.add_argument('--timeout', type=int, default=180)
    main(parser.parse_args())
