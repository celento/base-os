#!/usr/bin/env python3
"""Make or refresh the DOS disk: a FAT32 image with DOSBox and DOS programs.

    python3 tools/dos_disk.py build/baseos-dos.img dos-apps

The image holds /DOSBOX (DOSBox Staging from build/ports/dosbox, built by
python3 ports/build.py) and /DOS, which DOSBox mounts as drive C:. /DOS gets
the programs that ship with BaseOS (tools/dos_apps.py; --no-bundled leaves
them out) and everything in the apps folder. A new image is created only when it
does not exist, so files saved inside DOS stay on it; later runs add new
files from the apps folder and refresh DOSBox, but never overwrite a file
already in /DOS. Needs mtools and mkfs.fat (dosfstools).
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile

import dos_apps

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOSBOX = ROOT / 'build/ports/dosbox'
SIZE_MIB = 256
AUTOEXEC = """[autoexec]
mount c /disk1/DOS
c:
"""
ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1')


def mtools(*arguments):
    subprocess.run(arguments, check=True, env=ENV)


def copy_new(*arguments):
    """mcopy that skips files already on the disk. mcopy then exits with 1
    and prints nothing, so only a message on stderr means a real failure."""
    result = subprocess.run(['mcopy', '-s', '-D', 's', *arguments], env=ENV, stderr=subprocess.PIPE, text=True)
    if result.returncode and (result.returncode != 1 or result.stderr.strip()):
        raise SystemExit(f'mcopy failed: {result.stderr.strip() or result.returncode}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=pathlib.Path)
    parser.add_argument('apps', type=pathlib.Path, nargs='?', help='folder of DOS programs to copy into /DOS')
    parser.add_argument('--no-bundled', dest='bundled', action='store_false',
                        help='leave out the DOS programs that ship with BaseOS')
    args = parser.parse_args()
    if not (DOSBOX / 'DOSBOX').exists():
        raise SystemExit('Build DOSBox first: python3 toolchain/build.py && python3 ports/build.py')
    if not args.image.exists():
        args.image.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['mkfs.fat', '-C', '-F', '32', '-n', 'DOS', str(args.image), str(SIZE_MIB * 1024)],
                       check=True, capture_output=True)
        mtools('mmd', '-i', str(args.image), '::DOS')
        print(f'created {args.image} ({SIZE_MIB} MiB)')
    with tempfile.TemporaryDirectory() as temporary:
        # DOSBox itself is refreshed every time; its settings file mounts /DOS as C:.
        staged = pathlib.Path(temporary) / 'DOSBOX'
        shutil.copytree(DOSBOX, staged)
        conf = staged / 'dosbox-staging.conf'
        # The settings come from the source tree, so changing them needs no DOSBox rebuild.
        text = (ROOT / 'ports/dosbox/dosbox-staging.conf').read_text()
        conf.write_text(text[:text.index('[autoexec]')] + AUTOEXEC)
        mtools('mcopy', '-s', '-D', 'o', '-i', str(args.image), str(staged), '::')
    if args.bundled:
        with tempfile.TemporaryDirectory() as temporary:
            staged = dos_apps.stage(pathlib.Path(temporary) / 'DOS')
            copy_new('-i', str(args.image), *map(str, sorted(staged.iterdir())), '::DOS')
        print(f'bundled DOS programs are in /DOS: {", ".join(app["title"] for app in dos_apps.APPS)}')
    if args.apps and args.apps.is_dir():
        entries = sorted(args.apps.iterdir())
        if entries:
            copy_new('-i', str(args.image), *map(str, entries), '::DOS')
        print(f'copied {len(entries)} entries from {args.apps} into /DOS (existing files kept)')
    print('In BaseOS, open a DOS program from /Applications or the Launcher, or run start /disk1/DOSBOX/DOSBOX in Terminal')


if __name__ == '__main__':
    main()
