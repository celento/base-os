#!/usr/bin/env python3
"""The DOS programs that ship with BaseOS, and how they are staged.

    python3 tools/dos_apps.py OUTPUT

Each program comes from the FreeDOS package archive on GitLab, pinned to a
commit, and is cloned once into build/dos-apps-src. stage() lays them out the
way the DOS disk holds them under /DOS (drive C: in DOSBox):

    NETHACK/        the program, its documentation and its license
    NETHACK.BAT     changes into the folder and starts the program
    SOURCE/NETHACK/ the program's source code, as the package ships it

BaseOS has one launcher file per program, /Applications/<title>.dos, made by
src/lib/provision.c. Its text is the batch file's path, and opening it starts
DOSBox with that path. The titles and batch names in DOS_APP_LAUNCHERS in
provision.c must match APPS here (tests/test_dos_apps.py checks it).
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/dos-apps-src'
ARCHIVE = 'https://gitlab.com/FreeDOS/'
BIN = '/disk1/DOS'

# title: launcher name in /Applications. name: 8.3 folder and batch name.
# package: FreeDOS archive repository and commit. folder: the program folder
# inside the package. run: commands the batch file runs in that folder.
# extra: (package, path inside it) of further files the program needs.
APPS = [
    dict(title='FED Editor', name='FED', package=('editor/fed', 'b43ccaeb8394e292da7a1bedddd440d3f160dae2'),
         folder='EDIT/FED', license='GNU GPL 2', run=['FED.EXE'],
         extra=[('vitetris', 'GAMES/VITETRIS/CWSDPMI.EXE'), ('vitetris', 'GAMES/VITETRIS/CWSDPMI.DOC')]),
    dict(title='NetHack', name='NETHACK', package=('games/nethack', '7c20543b16f1551f56f57ec66c5c77556d26b22f'),
         folder='GAMES/NETHACK', license='NetHack General Public License', run=['NETHACK.EXE']),
    dict(title='Floppy Bird', name='FLPYBIRD', package=('games/flpybird', '0a3fc4df59c503385f60065971560cd3ce2357d2'),
         folder='GAMES/FLPYBIRD', license='MIT', run=['FLPYBIRD.COM']),
    dict(title='Vitetris', name='VITETRIS', package=('games/vitetris', '2cdd8485721a27ac617df969e569180a19bf597e'),
         folder='GAMES/VITETRIS', license='BSD 2-clause', run=['TETRIS.EXE']),
    dict(title='Sudoku86', name='SUDOKU86', package=('games/sudoku86', '89e81705449c02daf251450810fbbf53f5d945e6'),
         folder='GAMES/SUDOKU86', license='BSD 2-clause', run=['SUDOKU86.EXE']),
]


def package(path, commit):
    """A clean checkout of one FreeDOS archive package at commit."""
    tree = CACHE / pathlib.PurePosixPath(path).name
    if not (tree / '.git').exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '-q', str(tree)], check=True)
        subprocess.run(['git', '-C', str(tree), 'remote', 'add', 'origin', ARCHIVE + path + '.git'], check=True)
    head = subprocess.run(['git', '-C', str(tree), 'rev-parse', '-q', '--verify', 'HEAD'],
                          capture_output=True, text=True).stdout.strip()
    if head != commit:
        print(f'fetching {path} at {commit[:12]}', flush=True)
        subprocess.run(['git', '-C', str(tree), 'fetch', '-q', '--depth', '1', 'origin', commit], check=True)
        subprocess.run(['git', '-C', str(tree), 'checkout', '-q', '--force', commit], check=True)
    return tree


def packages():
    """Every package tree by its short name, fetched when missing."""
    trees = {}
    for app in APPS:
        path, commit = app['package']
        trees[pathlib.PurePosixPath(path).name] = package(path, commit)
    return trees


def batch(app):
    lines = ['@ECHO OFF', f'CD \\{app["name"]}', *app['run'], 'CD \\']
    return '\r\n'.join(lines) + '\r\n'


def stage(output):
    """Write the /DOS contents for every program into output."""
    output = pathlib.Path(output)
    trees = packages()
    for app in APPS:
        tree = trees[pathlib.PurePosixPath(app['package'][0]).name]
        target = output / app['name']
        shutil.copytree(tree / app['folder'], target, dirs_exist_ok=True)
        for source, path in app.get('extra', ()):
            shutil.copy(trees[source] / path, target / pathlib.PurePosixPath(path).name)
        # The package description names the license; LICENSE files stay as shipped.
        for info in sorted((tree / 'APPINFO').glob('*.LSM')):
            shutil.copy(info, target / info.name)
        # FAT names ignore case, so a program's own "license" file wins.
        if (tree / 'LICENSE').is_file() and not any(p.name.upper() == 'LICENSE' for p in target.iterdir()):
            shutil.copy(tree / 'LICENSE', target / 'LICENSE')
        if (tree / 'SOURCE').is_dir():
            shutil.copytree(tree / 'SOURCE', output / 'SOURCE', dirs_exist_ok=True)
        (output / f'{app["name"]}.BAT').write_bytes(batch(app).encode('ascii'))
    readme = ['DOS programs bundled with BaseOS', '',
              'Each folder holds one program with its documentation and license.',
              'SOURCE holds the source code of each program as FreeDOS ships it.', '']
    for app in APPS:
        path, commit = app['package']
        readme.append(f'{app["name"]:<9} {app["title"]} ({app["license"]}), {ARCHIVE}{path} at {commit}')
    (output / 'BUNDLED.TXT').write_bytes(('\r\n'.join(readme) + '\r\n').encode('ascii'))
    return output


if __name__ == '__main__':
    print(stage(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/dos-apps'))
