"""Offline file exchange. Stop QEMU first.

python3 tools/volume.py build/baseos-data.img info
python3 tools/volume.py build/baseos-data.img import picture.bmp /Pictures/picture.bmp
python3 tools/volume.py build/baseos-data.img export /Pictures/picture.bmp picture.bmp
python3 tools/volume.py build/baseos.img ls /docs
python3 tools/volume.py build/baseos.img mkdir /Projects

Legacy floppy snapshots and explicitly marked 16 MiB or 64 MiB data disks are supported.
Create a new data image with init_data.py, then boot BaseOS once before importing.
"""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import datetime
import fcntl
import hashlib
import os
from pathlib import Path
import stat
import struct
import tempfile
import zlib
from layout import constants

C = constants()
MAGIC = 0x46534f42
SECTOR_SIZE = 512
LEGACY_NODES = C['FS_LEGACY_NODES']
MAX_NODES = C['FS_MAX_NODES']
MAX_DEPTH = C['FS_MAX_DEPTH']
PATH_LEN = C['FS_PATH_LEN']
DATA_MARKER_MAGIC = C.get('DATA_MARKER_MAGIC', 0x44534f42)
DATA_MARKER_VERSION = C.get('DATA_MARKER_VERSION', 1)
DATA_DISK_SECTORS = C.get('DATA_DISK_SECTORS', 32768)
DATA_SLOT_SECTORS = C.get('DATA_SLOT_SECTORS', 16383)
DATA_FIRST_LBA = C.get('DATA_FIRST_LBA', 1)
DATA_SECOND_LBA = C.get('DATA_SECOND_LBA', 16384)
DATA_FILE_LIMIT = 2097152
DATA_TOTAL_LIMIT = (DATA_SLOT_SECTORS - 1) * SECTOR_SIZE - LEGACY_NODES * 40
LARGE_DATA_FILE_LIMIT = 16777216
LARGE_DATA_TOTAL_LIMIT = (C['DATA_LARGE_SLOT_SECTORS'] - 1) * SECTOR_SIZE - LEGACY_NODES * 40
EPOCH = datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc)


@dataclass(frozen=True)
class VolumeLayout:
    kind: str
    sectors: int
    slot_sectors: int
    lbas: tuple
    version: int
    file_limit: int
    total_limit: int
    node_limit: int
    marker_version: int = 0

    def capacity(self, count):
        """Preserve the full old v4 capacity; reserve extra records as needed."""
        if self.kind == 'data':
            return self.payload_limit - max(LEGACY_NODES, count) * 40
        return self.total_limit

    @property
    def payload_limit(self):
        return (self.slot_sectors - 1) * SECTOR_SIZE


FLOPPY_LAYOUT = VolumeLayout('floppy', C['DISK_SECTORS'], C['FS_DISK_SECTORS'],
                             (C['FS_DISK_LBA'], C['FS_SECOND_LBA']), 3, 16383,
                             (LEGACY_NODES - 1) * 16383, LEGACY_NODES)
DATA_LAYOUT = VolumeLayout('data', DATA_DISK_SECTORS, DATA_SLOT_SECTORS,
                           (DATA_FIRST_LBA, DATA_SECOND_LBA), 4,
                           DATA_FILE_LIMIT, DATA_TOTAL_LIMIT, MAX_NODES, DATA_MARKER_VERSION)
LARGE_DATA_LAYOUT = VolumeLayout('data', C['DATA_LARGE_DISK_SECTORS'], C['DATA_LARGE_SLOT_SECTORS'],
                                 (C['DATA_LARGE_FIRST_LBA'], C['DATA_LARGE_SECOND_LBA']), 5,
                                 LARGE_DATA_FILE_LIMIT, LARGE_DATA_TOTAL_LIMIT, MAX_NODES,
                                 C['DATA_LARGE_MARKER_VERSION'])
DATA_PROFILES = {'default': DATA_LAYOUT, 'large': LARGE_DATA_LAYOUT}


def data_layout(profile='default'):
    try:
        return DATA_PROFILES[profile]
    except KeyError:
        raise ValueError('Unknown data profile; choose default or large') from None


def data_marker(profile='default'):
    """The explicitly opted-in marker; no-argument calls retain exact v1 bytes."""
    layout = data_layout(profile)
    header = struct.pack('<6I', DATA_MARKER_MAGIC, layout.marker_version,
                         layout.sectors, layout.slot_sectors, *layout.lbas)
    return (header + struct.pack('<I', zlib.crc32(header))).ljust(SECTOR_SIZE, b'\0')


def disk_layout(data):
    """Recognize only exact known geometry and complete checksummed markers."""
    for profile, layout in DATA_PROFILES.items():
        if len(data) == layout.sectors * SECTOR_SIZE:
            if data[:SECTOR_SIZE] != data_marker(profile):
                raise ValueError('Invalid or missing data-disk marker; refusing this image')
            return layout
    if len(data) in (2880 * SECTOR_SIZE, C['DISK_SECTORS'] * SECTOR_SIZE):
        return FLOPPY_LAYOUT
    raise ValueError('Unrecognized image size; refusing this image')


def rolling(data):
    n = len(data)
    for byte in data:
        n = ((n << 1) + (n >> 31) + byte) & 0xffffffff
    return n


def decode(data, slot, layout=None):
    try:
        layout = layout or disk_layout(data)
    except ValueError:
        return None
    if slot not in (0, 1):
        return None
    start = layout.lbas[slot] * SECTOR_SIZE
    if len(data) < start + layout.slot_sectors * SECTOR_SIZE:
        return None
    magic, version, count, size, crc, gen, hcrc = struct.unpack_from('<7I', data, start)
    versions = (layout.version,) if layout.kind == 'data' else (1, 2, 3)
    if magic != MAGIC or version not in versions or not 1 <= count <= layout.node_limit:
        return None
    if version == 1 and slot:
        return None
    if version >= 2 and zlib.crc32(data[start:start + 24]) != hcrc:
        return None
    if size > layout.payload_limit:
        return None
    payload = data[start + SECTOR_SIZE:start + SECTOR_SIZE + size]
    if (rolling(payload) if version == 1 else zlib.crc32(payload)) != crc:
        return None
    nodes = {}
    pos = total = 0
    record = 40 if version >= 3 else 36
    for _ in range(count):
        if pos + record > len(payload):
            return None
        ident, parent, directory, app, _, length, raw = struct.unpack_from('<HhBBHI24s', payload, pos)
        modified = struct.unpack_from('<I', payload, pos + 36)[0] if version >= 3 else 0
        pos += record
        if (ident >= layout.node_limit or ident in nodes or parent < -1 or parent >= layout.node_limit
                or directory > 1 or app > 1 or (directory and app)):
            return None
        if length > layout.file_limit or pos + length > len(payload) or ((directory or app) and length):
            return None
        total += length
        if total > layout.capacity(count) or b'\0' not in raw:
            return None
        try:
            name = raw.split(b'\0', 1)[0].decode('latin1')
        except UnicodeError:
            return None
        if '/' in name or (ident and name in ('', '.', '..')):
            return None
        nodes[ident] = dict(name=name, parent=parent, directory=directory, app=app,
                            data=payload[pos:pos + length], modified=modified)
        pos += length
    if (pos != size or 0 not in nodes or nodes[0]['name']
            or nodes[0]['parent'] != -1 or not nodes[0]['directory']):
        return None
    siblings = set()
    for ident, node in nodes.items():
        if not ident:
            continue
        key = (node['parent'], node['name'])
        if key in siblings:
            return None
        siblings.add(key)
        walk, seen, path_bytes = ident, set(), 0
        while walk:
            if walk in seen:
                return None
            seen.add(walk)
            path_bytes += len(nodes[walk]['name']) + 1
            if len(seen) > MAX_DEPTH or path_bytes >= PATH_LEN:
                return None
            walk = nodes[walk]['parent']
            if walk not in nodes or not nodes[walk]['directory']:
                return None
    return (gen if version >= 2 else 0), nodes


def load(data):
    layout = disk_layout(data)
    valid = [(i, result) for i in range(2)
             if (result := decode(data, i, layout)) is not None]
    if not valid:
        raise ValueError('No valid filesystem snapshot; boot BaseOS once to initialize a blank disk.')
    slot, (gen, nodes) = valid[0]
    for i, (other, n) in valid[1:]:
        if 0 < ((other - gen) & 0xffffffff) < 0x80000000:
            slot, gen, nodes = i, other, n
    return slot, gen, nodes


def resolve(nodes, path):
    if len(path) >= PATH_LEN:
        raise ValueError(f'Path must be shorter than {PATH_LEN} bytes')
    ident = 0
    for name in path.split('/'):
        if name in ('', '.'):
            continue
        if name == '..':
            ident = max(0, nodes[ident]['parent'])
            continue
        if not nodes[ident]['directory']:
            raise ValueError('Not a directory')
        ident = next((i for i, n in nodes.items() if n['parent'] == ident and n['name'] == name), -1)
        if ident < 0:
            raise ValueError(f'Path not found: {path}')
    return ident


def check_image_identity(image, original):
    if not os.path.samestat(os.fstat(original.fileno()), image.stat()):
        raise ValueError('Image changed while opening it; retry with QEMU stopped')


@contextmanager
def locked_image(image):
    """Use QEMU-compatible byte-range locks, including during atomic replacement."""
    with image.open('r+b') as original:
        fcntl.lockf(original, fcntl.LOCK_EX | fcntl.LOCK_NB)
        check_image_identity(image, original)
        yield original


def sync_directory(directory):
    fd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def encode_snapshot(nodes, layout, generation):
    """Serialize recognized profile bounds; callers validate the entire graph
    with decode before publishing. Latin-1 round-trips the kernel's byte names."""
    if layout not in (FLOPPY_LAYOUT, DATA_LAYOUT, LARGE_DATA_LAYOUT):
        raise ValueError('Unrecognized snapshot layout')
    if not 1 <= len(nodes) <= layout.node_limit:
        raise ValueError('No free file slots')
    if any(len(n['data']) > layout.file_limit for n in nodes.values()):
        raise ValueError(f'File exceeds {layout.file_limit:,} bytes')
    allowance = layout.capacity(len(nodes))
    if sum(len(n['data']) for n in nodes.values()) > allowance:
        raise ValueError(f'Volume full: total file data exceeds {allowance:,} bytes')
    payload = bytearray()
    for ident, n in sorted(nodes.items()):
        name = n['name'].encode('latin1')
        if len(name) >= 24 or b'\0' in name:
            raise ValueError('Invalid file name')
        payload += struct.pack('<HhBBHI24sI', ident, n['parent'], n['directory'], n['app'], 0,
                               len(n['data']), name, n['modified']) + n['data']
    if len(payload) > layout.payload_limit:
        raise ValueError('Volume full')
    header = struct.pack('<6I', MAGIC, layout.version, len(nodes), len(payload),
                         zlib.crc32(payload), generation & 0xffffffff)
    header += struct.pack('<I', zlib.crc32(header))
    return header, payload


def commit(image, data, slot, generation, nodes, *, original=None):
    """Back up and replace only the older snapshot, never the current snapshot.

    Callers outside the CLI may use this directly; the source is locked and
    checked against the supplied bytes before any backup or replacement.
    """
    image = Path(image)
    if original is None:
        with locked_image(image) as source:
            if source.read() != data:
                raise ValueError('Image changed since it was read; refusing stale update')
            return commit(image, data, slot, generation, nodes, original=source)
    check_image_identity(image, original)
    layout = disk_layout(data)
    if len(data) != layout.sectors * SECTOR_SIZE:
        raise ValueError('Run make to upgrade the image before importing.')
    actual_slot, actual_generation, _ = load(data)
    if (slot, generation) != (actual_slot, actual_generation):
        raise ValueError('Snapshot changed; refusing stale update')
    header, payload = encode_snapshot(nodes, layout, generation + 1)
    start = layout.lbas[1 - slot] * SECTOR_SIZE
    updated = bytearray(data)
    updated[start:start + SECTOR_SIZE] = header.ljust(SECTOR_SIZE, b'\0')
    updated[start + SECTOR_SIZE:start + SECTOR_SIZE + len(payload)] = payload
    if decode(updated, 1 - slot, layout) is None:
        raise ValueError('Invalid filesystem snapshot; refusing update')
    backup = image.with_name(image.name + '.' + hashlib.sha256(data).hexdigest()[:16] + '.bak')
    try:
        with backup.open('xb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except FileExistsError:
        if backup.read_bytes() != data:
            raise ValueError('Backup already exists with different contents; refusing update')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=image.parent, delete=False) as f:
            temporary = Path(f.name)
            fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            os.fchmod(f.fileno(), stat.S_IMODE(os.fstat(original.fileno()).st_mode))
            f.write(updated)
            f.flush()
            os.fsync(f.fileno())
            check_image_identity(image, original)
            os.replace(temporary, image)
            temporary = None
            sync_directory(image.parent)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def show_info(data):
    layout = disk_layout(data)
    print(f'Type: {layout.kind} ({len(data):,} bytes)')
    print(f'Snapshot version: {layout.version}')
    print(f'File limit: {layout.file_limit:,} bytes')
    print(f'Snapshot payload capacity: {layout.payload_limit:,} bytes')
    print(f'Node limit: {layout.node_limit} (includes root, folders, and applications)')
    if layout.kind == 'data' and not any(data[SECTOR_SIZE:]):
        print('State: blank marked data disk; boot BaseOS once before importing')
        return
    slot, generation, nodes = load(data)
    used = sum(len(n['data']) for n in nodes.values())
    print(f'Active snapshot: {slot}; generation: {generation}')
    print(f'Used: {used:,} file bytes; {len(nodes)} nodes')
    allowance = layout.capacity(len(nodes))
    print(f'Total file-data limit: {allowance:,} bytes')
    print(f'Remaining file-data allowance: {allowance - used:,} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('info')
    p = sub.add_parser('ls')
    p.add_argument('path', default='/', nargs='?')
    p = sub.add_parser('import')
    p.add_argument('host', type=Path)
    p.add_argument('path')
    p.add_argument('--replace', action='store_true')
    p = sub.add_parser('export')
    p.add_argument('path')
    p.add_argument('host', type=Path)
    p.add_argument('--replace', action='store_true')
    p = sub.add_parser('mkdir')
    p.add_argument('path')
    a = parser.parse_args()
    try:
        with locked_image(a.image) as f:
            data = f.read()
            layout = disk_layout(data)
            if a.command == 'info':
                show_info(data)
                return
            slot, gen, nodes = load(data)
            if a.command == 'ls':
                ident = resolve(nodes, a.path)
                for n in nodes.values():
                    if n['parent'] == ident:
                        stamp = str(EPOCH + datetime.timedelta(seconds=n['modified'])) if n['modified'] else 'unknown'
                        print(f"{'dir' if n['directory'] else 'file':4} {len(n['data']):8} {stamp} {n['name']}")
            elif a.command == 'export':
                n = nodes[resolve(nodes, a.path)]
                if n['directory'] or n['app']:
                    raise ValueError('Not a data file')
                if a.host.exists() and os.path.samefile(a.host, a.image):
                    raise ValueError('Export destination is the disk image')
                with a.host.open('wb' if a.replace else 'xb') as out:
                    out.write(n['data'])
            else:
                if len(a.path) >= PATH_LEN:
                    raise ValueError(f'Path must be shorter than {PATH_LEN} bytes')
                parent_path, _, name = a.path.rstrip('/').rpartition('/')
                parent = resolve(nodes, parent_path)
                if (not nodes[parent]['directory'] or name in ('', '.', '..')
                        or '\0' in name or not 0 < len(name.encode('ascii')) < 24):
                    raise ValueError('Invalid destination')
                ident = next((i for i, n in nodes.items() if n['parent'] == parent and n['name'] == name), None)
                if ident is not None and (a.command == 'mkdir' or not a.replace or nodes[ident]['directory'] or nodes[ident]['app']):
                    raise ValueError('Destination exists; use --replace for a data file')
                if ident is None:
                    ident = next((i for i in range(1, layout.node_limit) if i not in nodes), None)
                if ident is None:
                    raise ValueError('No free file slots')
                content = b''
                if a.command == 'import':
                    with a.host.open('rb') as host:
                        content = host.read(layout.file_limit + 1)
                if len(content) > layout.file_limit:
                    raise ValueError(f'File exceeds {layout.file_limit:,} bytes')
                nodes[ident] = dict(name=name, parent=parent, directory=int(a.command == 'mkdir'), app=0,
                                   data=content, modified=int((datetime.datetime.now(datetime.timezone.utc) - EPOCH).total_seconds()))
                commit(a.image, data, slot, gen, nodes, original=f)
    except (OSError, ValueError, UnicodeError) as e:
        parser.exit(1, f'volume: {e}\n')


if __name__ == '__main__':
    main()
