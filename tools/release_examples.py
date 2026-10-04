"""Deterministic, explicitly selected BEX2 examples for fresh release disks only."""
import hashlib
import pathlib
import re
import subprocess
import sys

SPECS = (('workspace-array.bex', 'examples/c/workspace_array.c'),
         ('workspace-index.bex', 'examples/c/workspace_index.c'))
INPUTS = ('tools/build_app.py', 'sdk/start2.c', 'sdk/app2.ld', 'sdk/baseos.h',
          'sdk/baseos_abi.h', 'sdk/baseos_app2.h', 'sdk/baseos_executable.h',
          'examples/c/workspace_common.h')
GUIDE_NAME = 'Workspace guide.txt'
GUIDE = b'''NATIVE WORKSPACE EXAMPLES\n\nThese opt-in BEX2 C examples each use a private 1 MiB workspace and\na 64 KiB stack. Open workspace-array.bex or workspace-index.bex from\n/Programs or Ctrl+Space. They read /Documents/stats-sample.txt and\nwrite a small report under /Documents using versioned files and an\nowned background save. Program exit retains its report and terminal output.\n\nThe array example checks every workspace word; the index example\nindexes lines and checks all workspace storage. They are SDK examples,\nnot a general heap or new document editor. Existing BEX1 apps retain\ntheir 64 KiB total memory and unchanged binary contract.\n\nFor a different input, use Terminal: start /Programs/workspace-index.bex\n/Documents/your-file.txt (on one line). File arguments are optional.\nTwo 1 MiB examples fit the default profile, but current free pages and\nthe eight-window limit still apply. A memory-capacity message means\nclose an unused app and retry; another app's contents are preserved.\nBEX2 does not run under synchronous exec or older BEX1-only kernels.\n\nSource, build commands and exact limits: source/docs/BEX2_FORMAT.md\nand source/docs/NATIVE_PLATFORM_ABI.md in the release archive.\n'''


def require_workspace_loader(source):
    source = pathlib.Path(source)
    header = (source / 'src/program.h').read_text()
    enabled = re.findall(r'^\s*#\s*define\s+BASEOS_BEX2_ENABLED\s+([01])\s*$', header, re.M)
    if enabled != ['1']:
        raise ValueError('Workspace examples require the explicit source-enabled BEX2 loader')


def build_workspace_examples(source, destination):
    source, destination = pathlib.Path(source), pathlib.Path(destination)
    require_workspace_loader(source)
    # This helper owns a new staging directory, never an existing saved disk or output.
    destination.mkdir(parents=True, exist_ok=False)
    result = []
    for name, relative in SPECS:
        output = destination / name
        subprocess.run([sys.executable, str(source / 'tools/build_app.py'),
                        str(source / relative), str(output), '--format', 'bex2',
                        '--workspace-bytes', '1048576', '--stack-bytes', '65536',
                        '--required-abi-minor', '1'], check=True)
        data = output.read_bytes()
        result.append({'name': name, 'source': relative, 'format': 'BEX2-v1',
                       'workspace_bytes': 1048576, 'stack_bytes': 65536,
                       'required_abi': [1, 1], 'bytes': len(data),
                       'sha256': hashlib.sha256(data).hexdigest(),
                       'source_sha256': {p: hashlib.sha256((source / p).read_bytes()).hexdigest()
                                         for p in (*INPUTS, relative)}})
    return result
