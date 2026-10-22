"""Real-QEMU checks that a new data disk gets its sample files on first boot.

Build first, then: python3 tools/sample_files_test.py build
Uses only disposable images in a temporary directory.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from init_data import initialize
from qemu_session import DesktopSession
import sample_pack
import volume


def symbols(build):
    lines = subprocess.check_output(['nm', '-n', str(build / 'kernel.elf')], text=True).splitlines()
    return {p[2]: int(p[0], 16) for line in lines if len(p := line.split()) == 3}


def first_boot(build, disk, memory='64M'):
    """Boot once until the new disk is saved; returns the serial log."""
    touched = symbols(build)['fs_touched']
    with DesktopSession(build, 'samples', extra=['-m', memory, '-nic', 'none',
                        '-drive', f'file={disk},format=raw,index=0,if=ide']) as guest:
        guest.boot()
        guest.wait(lambda: struct.unpack('<I', guest.memory(touched, 4))[0] == 0,
                   'new data disk did not save')
        return guest.log.read_text()


def tree(disk):
    _, _, nodes = volume.load(disk.read_bytes())
    def path(ident):
        parts = []
        while ident:
            parts.append(nodes[ident]['name'])
            ident = nodes[ident]['parent']
        return '/'.join(reversed(parts))
    return {path(i): n for i, n in nodes.items() if i}


def check_samples(build, work, profile, memory):
    disk = work / f'{profile}.img'
    files = sample_pack.default_files()
    assert initialize(disk, profile=profile, samples=files)
    log = first_boot(build, disk, memory)
    assert 'FS sample files added\n' in log, log
    nodes = tree(disk)
    for name, data in files.items():
        node = nodes.get(name)
        assert node and not node['directory'] and node['data'] == data, name
    for folder in ('Documents/Sample', 'Pictures/Sample', 'Media/Sample'):
        assert nodes[folder]['directory'], folder
    # The built-in defaults are still there beside the samples.
    for name in ('Documents/budget.bsh', 'Media/chime.wav', 'readme.txt', 'Programs/hello.bex'):
        assert name in nodes, name
    print(f'{profile}: {len(files)} sample files added byte-exact')


def check_damaged(build, work):
    disk = work / 'damaged.img'
    assert initialize(disk, samples=sample_pack.default_files())
    data = bytearray(disk.read_bytes())
    first, _ = sample_pack.locate(data)
    data[first * 512 + 100] ^= 0xFF
    disk.write_bytes(data)
    log = first_boot(build, disk)
    assert 'FS sample pack unreadable; skipped\n' in log, log
    nodes = tree(disk)
    assert not any(name.endswith('/Sample') for name in nodes), sorted(nodes)
    assert 'Documents/budget.bsh' in nodes
    print('damaged pack: skipped, disk set up normally')


def check_plain(build, work):
    disk = work / 'plain.img'
    assert initialize(disk)
    log = first_boot(build, disk)
    assert 'FS sample' not in log, log
    assert not any(name.endswith('/Sample') for name in tree(disk))
    print('disk without a pack: no samples')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    build = parser.parse_args().build.resolve()
    with tempfile.TemporaryDirectory(prefix='baseos-samples-') as temp:
        work = Path(temp)
        check_samples(build, work, 'default', '64M')
        check_samples(build, work, 'large', '128M')
        check_damaged(build, work)
        check_plain(build, work)
    print('sample files test passed')


if __name__ == '__main__':
    sys.exit(main())
