"""Run the normal functional checkpoint suite and retain one log per command.

No saved disk images are booted by these runners. `--extended` also checks the
large storage/media fixtures. Requires the normal build and test dependencies.
"""
import argparse
import datetime
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(build, output, extended=False):
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    # LeakSanitizer is unavailable under ptrace; ASan/UBSan stay enabled.
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    commands = [('build', ['make', '-j4']), ('host', ['make', 'test'])]
    tests = ['extent', 'ui', 'input', 'display', 'editor_input', 'task_input',
             'desktop_apps', 'images_input', 'download_input', 'sysmon_input', 'browser_download_input', 'editor_close', 'writer_input', 'writer_guide', 'writer_lifecycle', 'writer_search', 'writer_binding', 'docstats_input', 'file_clipboard_input', 'save_folder_input', 'node_capacity_input']
    if extended:
        tests += ['network', 'browser', 'download', 'data_volume', 'editor',
                  'session_draft', 'native_sdk', 'node_capacity', 'kernel_space', 'storage_audio', 'task_media', 'video', 'video_input', 'demo', 'desktop_workload']
    commands += [(name, [sys.executable, 'tools/' + name + '_test.py', str(build)]) for name in tests]
    if extended:
        commands += [('mpeg_av_48', [sys.executable, 'tools/mpeg_av_test.py', str(build), '--rate', '48000']),
                     ('mpeg_av_controls', [sys.executable, 'tools/mpeg_av_test.py', str(build), '--controls'])]
    report = {'started': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'commands': [],
              'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()}
    for name, command in commands:
        print(name + ': running', flush=True)
        log = output / (name + '.log')
        begin = datetime.datetime.now(datetime.timezone.utc)
        with log.open('w') as stream:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
        elapsed = (datetime.datetime.now(datetime.timezone.utc) - begin).total_seconds()
        report['commands'].append({'name': name, 'command': command, 'exit_code': result.returncode,
                                   'seconds': elapsed, 'log': log.name})
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{name}: {"PASS" if result.returncode == 0 else "FAIL"} ({elapsed:.1f}s)', flush=True)
        if result.returncode:
            print(log.read_text()[-6000:])
            raise SystemExit(result.returncode)
    report['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report['passed'] = True
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Checkpoint suite passed. Evidence:', output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path, nargs='?', default=ROOT / 'build')
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--extended', action='store_true')
    args = parser.parse_args()
    destination = args.output or pathlib.Path(tempfile.gettempdir()) / ('baseos-regression-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    run(args.build.resolve(), destination.resolve(), args.extended)
