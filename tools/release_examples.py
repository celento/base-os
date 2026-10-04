"""Deterministic, explicitly selected SDK examples for fresh release disks only."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from volume import load as load_volume, resolve as resolve_volume

SPECS = (('workspace-array.bex', 'examples/c/workspace_array.c'),
         ('workspace-index.bex', 'examples/c/workspace_index.c'))
INPUTS = ('tools/build_app.py', 'sdk/start2.c', 'sdk/app2.ld', 'sdk/baseos.h',
          'sdk/baseos_abi.h', 'sdk/baseos_app2.h', 'sdk/baseos_executable.h',
          'examples/c/workspace_common.h')
GUIDE_NAME = 'Workspace guide.txt'
GUIDE = b'''NATIVE WORKSPACE EXAMPLES\n\nThese opt-in BEX2 C examples each use a private 1 MiB workspace and\na 64 KiB stack. Open workspace-array.bex or workspace-index.bex from\n/Programs or Ctrl+Space. They read /Documents/stats-sample.txt and\nwrite a small report under /Documents using versioned files and an\nowned background save. Program exit retains its report and terminal output.\n\nThe array example checks every workspace word; the index example\nindexes lines and checks all workspace storage. They are SDK examples,\nnot a general heap or new document editor. Existing BEX1 apps retain\ntheir 64 KiB total memory and unchanged binary contract.\n\nFor a different input, use Terminal: start /Programs/workspace-index.bex\n/Documents/your-file.txt (on one line). File arguments are optional.\nTwo 1 MiB examples fit the default profile, but current free pages and\nthe eight-window limit still apply. A memory-capacity message means\nclose an unused app and retry; another app's contents are preserved.\nBEX2 does not run under synchronous exec or older BEX1-only kernels.\n\nSource, build commands and exact limits: source/docs/BEX2_FORMAT.md\nand source/docs/NATIVE_PLATFORM_ABI.md in the release archive.\n'''


POINTER_SPECS = (('pointer.bex', 'examples/c/pointer.c'),)
POINTER_INPUTS = ('tools/build_app.py', 'sdk/start.c', 'sdk/app.ld',
                  'sdk/baseos.h', 'sdk/baseos_abi.h', 'sdk/baseos_executable.h',
                  'examples/c/pointer_backend.h')
POINTER_SERVICE_INPUTS = ('Makefile', 'src/kernel.c', 'src/process.c',
                          'src/native_ui.c', 'src/native_ui.h', 'src/canvas_view.c',
                          'src/canvas_view.h', 'src/input_ingress.c', 'src/input_ingress.h')
POINTER_GUIDE_NAME = 'Pointer guide.txt'
POINTER_GUIDE = b'NATIVE POINTER SDK EXAMPLE\n\nOpen pointer.bex from /Programs or Ctrl+Space, or use Terminal:\nstart /Programs/pointer.bex\n\nPointer draws inside its existing hosted Terminal canvas. It requires\nthe installed hosted UI service, successful PS/2 initialization and a\nbound desktop task. Synchronous exec is unsupported. The app checks\nthe service at runtime and prints POINTER UNSUPPORTED if unavailable.\n\nHold left or right inside the drawing area for separate ink colors;\nhold both for a chord. Only ordinary final button release commits a\nstroke. Losing focus, overlays, minimize or geometry changes cancel\nthe unfinished preview. RESET/CANCEL never commit a partial stroke.\nRelease all buttons before beginning a fresh gesture after cancellation.\n\nR toggles 160x100 / 320x200 and clears; C clears the ink.\nO releases and reopens the endpoint; P prints Terminal status.\nQ or Esc exits. Status shows coordinates, sequence, reset/cancel counts.\n\nThis is a BEX1 SDK drawing/input demonstration. It does not save a\ndocument or create an independent native window. Closing loses ink.\nBEX1 memory limits and the five original frozen BEX1 examples are unchanged.\nSource and service limits: source/examples/c/pointer.c and\nsource/docs/NATIVE_UI.md in the release archive.\n'


def source_hashes(source, inputs):
    return {p: hashlib.sha256((source / p).read_bytes()).hexdigest() for p in inputs}


def require_pointer_service(source):
    """Recognize the known source-installed adapter, not global runtime support.

    This deliberately narrow wiring check must be reviewed if installation
    changes. Actual availability still depends on PS/2 and a bound desktop task;
    the example negotiates its UI capabilities at runtime.
    """
    source = pathlib.Path(source)
    # Both reviewed implementations scope bit 6 to a bound, input-enabled
    # desktop task. In the owned-window implementation its alternative bit is
    # selected by the executable's required launch flag, never globally ORed.
    hosted_feature_wirings = (
        'if(current_task&&current_task->bound&&native_ui_available())info.features|=BOS_FEATURE_HOSTED_UI;',
        'if(current_task&&current_task->bound&&native_ui_available()){'
        'info.features|=(current_task->plan.flags&BOS_BEX2_FLAG_NATIVE_WINDOW_V1)?'
        'BOS_FEATURE_OWNED_NATIVE_WINDOW:BOS_FEATURE_HOSTED_UI;}',
    )
    required = {
        'src/kernel.c': ('if(mouse_ok){const NativeUiHooks native_hooks={native_host_snapshot,native_host_acquired,native_host_focus};native_ui_init(&native_hooks);}',),
        'src/process.c': (
            'if(!current_task||!current_task->bound||!native_ui_available())return BOS_E_UNSUPPORTED;',
            'else if(call==BOS_CALL_UI)r[7]=(unsigned)native_ui_call(r,a,b,c,d,e);'),
        'src/native_ui.c': ('int native_ui_available(void){return hooks.snapshot&&hooks.acquired&&hooks.focus;}',),
        'sdk/baseos_abi.h': ('#define BOS_FEATURE_HOSTED_UI (1u<<6)', 'BOS_CALL_MEMORY_INFO=28,BOS_CALL_UI=29};', '#define BOS_UI_MAJOR 1u'),
    }
    try:
        makefile = (source / 'Makefile').read_text()
        sources = re.findall(r'^CSRC\s*=\s*(.*)$', makefile, re.M)
        if len(sources) != 1 or not all(name in sources[0].split()
                                      for name in ('native_ui.c', 'canvas_view.c', 'input_ingress.c')):
            raise ValueError('Hosted UI service is not in the known source build')
        for name, fragments in required.items():
            code = re.sub(r'/\*.*?\*/|//[^\n]*', '', (source / name).read_text(), flags=re.S)
            code = re.sub(r'\s+', '', code)
            if any(code.count(re.sub(r'\s+', '', part)) != 1 for part in fragments):
                raise ValueError('Pointer example requires known source-installed hosted UI wiring: ' + name)
            if name == 'src/process.c' and (
                    sum(code.count(part) for part in hosted_feature_wirings) != 1 or
                    code.count('BOS_FEATURE_HOSTED_UI') != 1):
                raise ValueError('Pointer example requires known context-scoped hosted UI feature wiring')
    except OSError as error:
        raise ValueError('Pointer example requires the source-installed hosted UI service') from error


def build_pointer_example(source, destination):
    source, destination = pathlib.Path(source), pathlib.Path(destination)
    require_pointer_service(source)
    result = _build_examples(source, destination, POINTER_SPECS, POINTER_INPUTS,
                             ['--format', 'bex1'], {'format': 'BEX1',
                             'required_features': ['BOS_FEATURE_HOSTED_UI'],
                             'required_ui': {'major': 1, 'capabilities': [
                                 'HOSTED_CANVAS', 'POINTER', 'IMPLICIT_CAPTURE',
                                 'BOUNDED_WAIT', 'LEGACY_KEY_READINESS'],
                                 'context': 'bound desktop task; PS/2-installed hosted Terminal canvas'}})
    for record in result:
        record['service_source_sha256'] = source_hashes(source, POINTER_SERVICE_INPUTS)
    return result


def require_workspace_loader(source):
    source = pathlib.Path(source)
    header = (source / 'src/program.h').read_text()
    enabled = re.findall(r'^\s*#\s*define\s+BASEOS_BEX2_ENABLED\s+([01])\s*$', header, re.M)
    if enabled != ['1']:
        raise ValueError('Workspace examples require the explicit source-enabled BEX2 loader')


def _build_examples(source, destination, specs, inputs, arguments, metadata):
    # This helper owns a new staging directory, never an existing saved disk or output.
    destination.mkdir(parents=True, exist_ok=False)
    result = []
    for name, relative in specs:
        output = destination / name
        subprocess.run([sys.executable, str(source / 'tools/build_app.py'),
                        str(source / relative), str(output), *arguments], check=True)
        data = output.read_bytes()
        result.append(dict(metadata, name=name, source=relative, bytes=len(data),
                           sha256=hashlib.sha256(data).hexdigest(),
                           source_sha256=source_hashes(source, (*inputs, relative))))
    return result


def build_workspace_examples(source, destination):
    source, destination = pathlib.Path(source), pathlib.Path(destination)
    require_workspace_loader(source)
    return _build_examples(source, destination, SPECS, INPUTS,
                           ['--format', 'bex2', '--workspace-bytes', '1048576',
                            '--stack-bytes', '65536', '--required-abi-minor', '1'],
                           {'format': 'BEX2-v1', 'workspace_bytes': 1048576,
                            'stack_bytes': 65536, 'required_abi': [1, 1]})


EXAMPLE_GROUPS = (
    ('native_workspace_examples', SPECS, build_workspace_examples, GUIDE_NAME, GUIDE),
    ('native_pointer_examples', POINTER_SPECS, build_pointer_example, POINTER_GUIDE_NAME, POINTER_GUIDE),
)


def verify_release_examples(root, manifest, profiles, destination):
    """Source-only executable rebuilds and exact program/guide copies per profile.

    Absent keys and empty lists retain support for old, unselected archives.
    Rebuilt declarations are compared in full before any metadata-supplied path
    is used, rejecting unknown names, formats, requirements and malformed records.
    """
    root, destination = pathlib.Path(root), pathlib.Path(destination)
    counts, expected, declared_names = {}, [], set()
    for key, specs, builder, guide_name, guide in EXAMPLE_GROUPS:
        records = manifest.get(key, [])
        if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
            raise ValueError('Invalid native example declaration: ' + key)
        counts[key] = len(records)
        if not records:
            continue
        if [record.get('name') for record in records] != [name for name, _ in specs]:
            raise ValueError('Unknown or incomplete native example declaration: ' + key)
        rebuilt_dir = destination / key
        rebuilt_records = builder(root / 'source', rebuilt_dir)
        if json.dumps(rebuilt_records, sort_keys=True) != json.dumps(records, sort_keys=True):
            raise ValueError('Rebuilt native examples or input provenance differ: ' + key)
        for record in rebuilt_records:
            rebuilt = (rebuilt_dir / record['name']).read_bytes()
            supplied = root / 'binaries/native' / record['name']
            if not supplied.is_file() or rebuilt != supplied.read_bytes():
                raise ValueError('Packaged native example differs from source rebuild: ' + record['name'])
            declared_names.add(record['name'])
            expected.append(('/Programs/' + record['name'], rebuilt))
        expected.append(('/Documents/' + guide_name, guide))
    native = root / 'binaries/native'
    supplied_names = {path.name for path in native.iterdir()} if native.is_dir() else set()
    if supplied_names != declared_names:
        raise ValueError('Unknown or undeclared native example binaries')
    if expected:
        for spec in profiles:
            _, _, nodes = load_volume((root / spec['data_image']).read_bytes())
            for path, content in expected:
                node = resolve_volume(nodes, path)
                if node < 0 or nodes[node]['directory'] or nodes[node]['app'] or nodes[node]['data'] != content:
                    raise ValueError('Fresh disk native example or guide differs: ' + spec['name'] + ':' + path)
    return counts
