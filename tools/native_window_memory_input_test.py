"""Bounded ordinary GUI physical-page admission gate; prepare-only by default.

Use --prepared pointing to a frozen native_window_input_test manifest. Existing
kernel, Pointer and document binaries are copied/read unchanged; only the NEW
page-budget.bex is built. After a separate explicit QEMU grant, --run consumes
this collector's prepared output. No debugger, guest memory, injected syscall,
fault probe, private process counter or live-volume observation is permitted.
"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess

from native_window_input_test import (ROOT, PROFILES, _bex2_header, build_app,
    declared_pages, file_record, make_volume, phase_run, require, save_json,
    stopped_volume, verify_inputs)

BUDGET_NAME = 'page-budget.bex'
BUDGET_SOURCE = ROOT/'tests/native_window_page_budget_app.c'
WORKSPACE_BYTES = 1048576
STACK_BYTES = 16384
REFUSAL = 'Cannot start: native backing memory is unavailable.'
MAX_BIG_INSTANCES = 6  # Observer+6 windows leaves a genuine free eighth slot.


def admission_plan(free_pages, commitment):
    """Derive the finite experiment from the observer's public pool snapshot."""
    require(isinstance(free_pages, int) and free_pages >= 0, 'Invalid public free-page count')
    require(isinstance(commitment, int) and commitment > 0, 'Invalid declared commitment')
    count, remainder = divmod(free_pages, commitment)
    require(1 <= count <= MAX_BIG_INSTANCES,
            'Profile does not permit a page-only refusal below the eight-window limit')
    return dict(admitted=count, remaining=remainder, failed_next_commitment=commitment,
                live_windows=count+1, free_wm_slots=8-count-1)


def stable_identity(state):
    """Fields that must survive refused launch without a new app incarnation."""
    return {name: state[name] for name in ('state', 'saved', 'verified', 'owned',
            'free', 'slot', 'phase', 'sleep', 'result')}


def is_budget(state, pages):
    return (state['owned'] == pages and state['verified'] == WORKSPACE_BYTES//4096 and
            state['state'] == 1 and state['saved'] == 0 and state['result'] == 0)


def refusal_visible(image, shell):
    """Known source-rendered UI text in the ordinary36px menu/status bar."""
    width = shell.bitmap(REFUSAL, 'ui').shape[1]
    # Its horizontal start follows variable-width menu labels. Search only the
    # source-defined baseline; do not infer focus/window state from private data.
    return any(shell.matches(image, x, 9, REFUSAL, 'ui')
               for x in range(max(0, image.shape[1]-width+1)))


def verify_memory_inputs(manifest):
    verify_inputs(manifest)
    origin = manifest['runtime_manifest']
    require(file_record(origin['path']) == origin, 'Parent runtime manifest changed after preparation')


def prepare(prepared, output, profiles=('default', 'large')):
    source_manifest = prepared/'manifest.json'
    base = json.loads(source_manifest.read_text()); verify_inputs(base)
    require(base.get('build_info', {}).get('dirty') is False, 'Use a frozen clean production build')
    require('window-document.bex' in base['apps'], 'Frozen document observer is missing')
    output.mkdir(parents=True, exist_ok=False)
    manifest = {key: deepcopy(base[key]) for key in ('build', 'apps', 'source', 'build_info', 'build_directory')}
    manifest.update(schema=1, status='PREPARING PAGE ADMISSION; NO GUEST RUN',
        preparation_only=True, runner_guest_qualified=False, runtime_manifest=file_record(source_manifest),
        runtime_revision=base['build_info']['revision'], profiles={},
        scope='Finite owned physical-page admission while a public-API observer remains live',
        observations='PS/2, source-decoded screenshots, ordinary public memory API, serial, stopped-volume bytes',
        not_claimed=['private process record exhaustion', 'allocation timing or allocator internal state'],
        expected_workspace_bytes=WORKSPACE_BYTES, expected_stack_bytes=STACK_BYTES)
    save_json(output/'manifest.json', manifest)
    try:
        manifest['collector_revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        manifest['source_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT))
        for relative in ('tools/native_window_memory_input_test.py', 'tests/native_window_page_budget_app.c',
                         'tests/native_window_document_app.c', 'tests/test_native_window_memory_collector.py'):
            manifest['source'][relative] = file_record(ROOT/relative)
        target = output/BUDGET_NAME
        build_app(BUDGET_SOURCE, target, format='bex2', window='native-v1',
                  workspace_bytes=WORKSPACE_BYTES, stack_bytes=STACK_BYTES)
        data = target.read_bytes(); header = _bex2_header(data)
        require(header[3] == 1 and header[10:14] == (WORKSPACE_BYTES, STACK_BYTES, 1, 2),
                'Page fixture must require the exact declared GUI BEX2 contract')
        fixture = file_record(target)
        fixture.update(owned_pages=declared_pages(data), workspace_pages=WORKSPACE_BYTES//4096,
                       source=file_record(BUDGET_SOURCE), verification='first and last byte of every workspace page; canvas VERIFY=256')
        manifest['apps'][BUDGET_NAME] = fixture; manifest['budget_fixture'] = fixture
        manifest['observer'] = deepcopy(manifest['apps']['window-document.bex'])
        require(manifest['observer']['owned_pages'] == 13, 'Expected immutable13-page document observer')
        apps = {name: Path(record['path']).read_bytes() for name, record in manifest['apps'].items()}
        for profile in profiles:
            folder = output/profile; folder.mkdir()
            manifest['profiles'][profile] = dict(ram_mib=PROFILES[profile], status='NOT RUN',
                initial_volume=make_volume(folder/'data.img', profile, apps))
        verify_memory_inputs(manifest)
        manifest['status'] = 'PREPARED PAGE ADMISSION; NO GUEST RUN'
    except Exception as error:
        manifest['status'] = 'PREPARATION FAILED; EVIDENCE RETAINED'; manifest['failure'] = repr(error); raise
    finally:
        save_json(output/'manifest.json', manifest)
    return manifest


def memory_phase(session, evidence, manifest):
    session.boot(); session.origin(); session.move(1275, 670)
    session.launch('window-document.bex'); session.key('alt-ret')
    baseline = evidence.document('observer-initial-public-pages')
    observer_pages = manifest['observer']['owned_pages']; pages = manifest['budget_fixture']['owned_pages']
    evidence.check('immutable observer reports13 owned pages', baseline['owned'] == observer_pages == 13,
                   state=baseline, app=manifest['observer']['sha256'])
    plan = admission_plan(baseline['free'], pages)
    evidence.report['admission_plan'] = plan; evidence.save()
    slots = []
    current = baseline
    for index in range(plan['admitted']):
        session.launch(BUDGET_NAME); session.key('alt-ret')
        current = evidence.document('budget-admitted-'+str(index+1))
        evidence.check('complete declared commitment for GUI'+str(index+1),
            is_budget(current, pages) and current['slot'] not in slots+[baseline['slot']] and
            current['free'] == baseline['free']-(index+1)*pages and current['phase'] == 0,
            state=current, declared_pages=pages, expected_free=baseline['free']-(index+1)*pages)
        slots.append(current['slot'])
    evidence.check('next complete commitment exceeds public free pool with WM capacity remaining',
        current['free'] < pages and len(slots)+1 < 8,
        state=current, live_app_windows=len(slots)+1, next_owned_pages=pages)
    before = current
    # The failed launch must return to this existing app. Do not maximize here:
    # toggling a still-maximized old owner would conceal a focus regression.
    session.launch(BUDGET_NAME)
    refused_image = evidence.frame('page-capacity-refusal-status')
    evidence.check('ordinary desktop shows backing-memory refusal', refusal_visible(refused_image, evidence.shell))
    after = evidence.document('page-capacity-refusal-existing-owner')
    evidence.check('refusal preserves visible current owner and state', stable_identity(after) == stable_identity(before),
                   before=before, after=after)
    session.key('m'); after = evidence.document('page-capacity-refusal-public-pages')
    evidence.check('failed complete admission consumes no continuing public pool pages',
                   stable_identity(after) == stable_identity(before), before=before, after=after)
    session.key('p'); focused = evidence.document('page-capacity-refusal-key-owner')
    evidence.check('failed launch keeps byte-key focus on same app',
        focused['slot'] == before['slot'] and focused['phase'] == before['phase']+1 and
        focused['free'] == before['free'] and is_budget(focused, pages), before=before, after=focused)
    closed_slot = slots.pop(); session.key('ctrl-w'); session.key('m')
    freed = evidence.document('one-budget-closed-public-pages')
    evidence.check('forced Close releases exactly one declared commitment',
        freed['free'] == before['free']+pages and freed['slot'] in slots+[baseline['slot']],
        state=freed, released_slot=closed_slot, declared_pages=pages)
    session.launch(BUDGET_NAME); session.key('alt-ret')
    retried = evidence.document('page-capacity-retry-succeeds')
    evidence.check('releasing one commitment permits fresh launch in reused slot',
        is_budget(retried, pages) and retried['slot'] == closed_slot and retried['phase'] == 0 and
        retried['free'] == before['free'], state=retried, released_slot=closed_slot)
    slots.append(retried['slot'])
    while slots:
        closed_slot = slots.pop(); session.key('ctrl-w'); session.key('m')
        current = evidence.document('cleanup-remaining-'+str(len(slots)))
        evidence.check('cleanup restores exact public pool after closing slot'+str(closed_slot),
            current['free'] == baseline['free']-len(slots)*pages and
            current['slot'] in slots+[baseline['slot']], state=current, remaining_budget_slots=list(slots))
    evidence.check('original observer keeps its slot state and exact baseline free pool',
        current['slot'] == baseline['slot'] and current['owned'] == baseline['owned'] and
        current['free'] == baseline['free'] and current['saved'] == baseline['saved'] and
        current['verified'] == baseline['verified'] and current['phase'] == baseline['phase'],
        before=baseline, after=current)
    session.key('p'); continued = evidence.document('original-observer-still-responsive')
    evidence.check('original observer remains responsive after admission refusal retry and cleanup',
        continued['slot'] == baseline['slot'] and continued['phase'] == baseline['phase']+1 and
        continued['free'] == baseline['free'], state=continued)
    session.key('q')


def run(output):
    manifest = json.loads((output/'manifest.json').read_text()); verify_memory_inputs(manifest)
    require(manifest['status'] == 'PREPARED PAGE ADMISSION; NO GUEST RUN', 'Use a fresh prepared page-capacity directory')
    require(not manifest['source_dirty'], 'Guest gate requires clean collector source preparation')
    manifest['status'] = 'RUNNING PAGE ADMISSION'; manifest['preparation_only'] = False
    save_json(output/'manifest.json', manifest)
    try:
        for profile, record in manifest['profiles'].items():
            verify_memory_inputs(manifest)
            disk = Path(record['initial_volume']['path'])
            require(file_record(disk) == record['initial_volume'], 'Page-capacity fixture changed before first boot')
            result = phase_run(Path(manifest['build_directory']), disk, record['ram_mib'], output/profile,
                               'page-admission', lambda session, evidence: memory_phase(session, evidence, manifest), manifest)
            verify_memory_inputs(manifest)
            record['stopped_volume'] = stopped_volume(disk, manifest, False)
            record['guest'] = result; record['status'] = 'PASSED PUBLIC PAGE ADMISSION'
            save_json(output/'manifest.json', manifest)
        manifest['status'] = 'PASSED PUBLIC PAGE ADMISSION'
    except Exception as error:
        manifest['status'] = 'FAILED; EVIDENCE RETAINED'; manifest['failure'] = repr(error); raise
    finally:
        save_json(output/'manifest.json', manifest)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, help='immutable parent native-window manifest directory')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profiles', nargs='+', choices=tuple(PROFILES), default=list(PROFILES))
    parser.add_argument('--run', action='store_true', help='run prepared gate ONLY AFTER a separate explicit QEMU grant')
    args = parser.parse_args()
    if args.run:
        require(args.prepared is None, '--run consumes only its already prepared manifest')
        report = run(args.output.resolve())
    else:
        require(args.prepared is not None, 'Preparation requires --prepared parent manifest')
        report = prepare(args.prepared.resolve(), args.output.resolve(), args.profiles)
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
