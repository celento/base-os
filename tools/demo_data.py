"""Seed only a freshly generated data disk with the original Harbor examples."""
import pathlib
import struct
import subprocess
from qemu_session import DesktopSession
from volume import data_layout, data_marker, load, resolve, commit

ROOT = pathlib.Path(__file__).resolve().parents[1]


def install(build, boot_image, data_image, epoch, profile='default'):
    """Boot an explicitly blank release disk, then atomically add original samples.

    Refuses all existing nonblank volume data. Called only for disposable release
    staging; ordinary user's disks continue to use the non-overwriting volume CLI.
    """
    build, boot_image, data_image = map(pathlib.Path, (build, boot_image, data_image))
    layout = data_layout(profile)
    blank = data_image.read_bytes()
    if len(blank) != layout.sectors * 512 or blank[:512] != data_marker(profile) or any(blank[512:]):
        raise ValueError('Demo seeding requires a newly generated blank data disk')
    symbols = {p[2]: int(p[0], 16) for line in subprocess.check_output(['nm', '-n', str(build / 'kernel.elf')], text=True).splitlines() if len(p := line.split()) == 3}
    with DesktopSession(build, 'demo-seed', image=boot_image.resolve(),
                        extra=['-m', '128M' if profile == 'large' else '64M',
                               '-drive', f'file={data_image.resolve()},format=raw,index=0,if=ide', '-nic', 'none']) as guest:
        guest.boot()
        guest.wait(lambda: struct.unpack('<I', guest.memory(symbols['fs_touched'], 4))[0] == 0,
                   'Fresh demo disk did not synchronize')
    data = data_image.read_bytes()
    slot, generation, nodes = load(data)
    media = resolve(nodes, '/Media')
    docs = resolve(nodes, '/Documents')
    modified = max(0, epoch - 946684800)
    examples = [('harbor.mp3', media, (ROOT / 'assets/examples/harbor.mp3').read_bytes()),
                ('harbor.mpg', media, (ROOT / 'assets/examples/harbor.mpg').read_bytes()),
                ('Media guide.txt', docs, b'''HARBOR MEDIA EXAMPLES\n\nPress Ctrl+Space and type harbor.mp3 or harbor.mpg to open a file.\nThe Media Player also lists both files alongside the short chime.\n\nHarbor MP3: 18 seconds of original synthesized music, stereo 44.1 kHz.\nHarbor MPEG: a 9-second sunset sailboat scene with the same soundtrack.\nUse Space to pause/resume; S stops. Volume uses the on-screen buttons.\nMinimize to continue playback in the background; closing stops playback.\n\nThese sounds and pictures were generated for BaseOS, with no downloaded\nrecordings or artwork. MIT license. Complete generator, provenance and\nlicense are in source/assets/examples/README.md in the source package.\n\nImport your own compatible files only while QEMU is stopped. See\nsource/docs/MEDIA.md, VIDEO.md and IMAGE_FORMATS.md for exact limits.\n''')]
    examples.append(('Writer guide.bwr', docs, (ROOT / 'assets/examples/writer-guide.bwr').read_bytes()))
    for name, parent, content in examples:
        if any(node['parent'] == parent and node['name'] == name for node in nodes.values()):
            raise ValueError('Fresh demo destination unexpectedly exists: ' + name)
        ident = next(i for i in range(1, layout.node_limit) if i not in nodes)
        nodes[ident] = dict(name=name, parent=parent, directory=0, app=0, data=content, modified=modified)
    before = set(data_image.parent.glob(data_image.name + '.*.bak'))
    commit(data_image, data, slot, generation, nodes)
    # This backup contains only the disposable, just-created seed state. It is
    # redundant in a fresh release archive and is not any user's saved disk.
    for backup in set(data_image.parent.glob(data_image.name + '.*.bak')) - before:
        backup.unlink()
    _, _, verified = load(data_image.read_bytes())
    for name, parent, content in examples:
        actual = next(n['data'] for n in verified.values() if n['parent'] == parent and n['name'] == name)
        if actual != content:
            raise ValueError('Demo import differs from source: ' + name)
    return [name for name, _, _ in examples]
