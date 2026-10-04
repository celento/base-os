"""Copy a stopped BaseOS volume into a NEW explicitly marked data image.

python3 tools/migrate_volume.py old-data.img new-large-data.img --profile large

Reads v1-v3 floppy, v4 default IDE and v5 large IDE snapshots. The source image
is locked and never written. Destination must not exist; there is no in-place
resize, overwrite, or automatic conversion. All node IDs, names, parents,
flags, modification timestamps and exact content are preserved. Two validated
snapshots are written before the new image is atomically published.
"""
import argparse
import fcntl
import os
from pathlib import Path
import tempfile
from volume import (SECTOR_SIZE, check_image_identity, data_layout, data_marker,
                    decode, encode_snapshot, load, locked_image, sync_directory)


def migrate(source, destination, *, profile='large'):
    source, destination = Path(source), Path(destination)
    layout = data_layout(profile)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError('Destination exists; choose a new image path')
    temporary = None
    with locked_image(source) as original:
        raw = original.read()
        _, generation, nodes = load(raw)
        updated = bytearray(layout.sectors * SECTOR_SIZE)
        updated[:SECTOR_SIZE] = data_marker(profile)
        for slot in (0, 1):
            header, payload = encode_snapshot(nodes, layout, generation + slot + 1)
            start = layout.lbas[slot] * SECTOR_SIZE
            updated[start:start + SECTOR_SIZE] = header.ljust(SECTOR_SIZE, b'\0')
            updated[start + SECTOR_SIZE:start + SECTOR_SIZE + len(payload)] = payload
            result = decode(updated, slot, layout)
            if result is None or result[1] != nodes:
                raise ValueError('Destination snapshot did not preserve every source node')
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as target:
                temporary = Path(target.name)
                fcntl.lockf(target, fcntl.LOCK_EX | fcntl.LOCK_NB)
                target.write(updated)
                target.flush()
                os.fsync(target.fileno())
                # Validate disk bytes too, before publishing any destination.
                target.seek(0)
                check = target.read()
                if check != updated or any(decode(check, slot, layout) is None for slot in (0, 1)):
                    raise ValueError('Destination verification failed')
                check_image_identity(source, original)
                original.seek(0)
                if original.read() != raw:
                    raise ValueError('Source changed during migration; destination not published')
                # Atomic no-replace publication, including concurrent creators
                # and dangling symlinks. Both locks remain held through fsync.
                os.link(temporary, destination)
                sync_directory(destination.parent)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return len(nodes), sum(len(n['data']) for n in nodes.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--profile', choices=('default', 'large'), default='large')
    args = parser.parse_args()
    try:
        count, size = migrate(args.source, args.destination, profile=args.profile)
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, f'migration: {error}\n')
    print(f'Created {args.destination}: {count} nodes, {size:,} exact file bytes; source unchanged.')
    if args.profile == 'large':
        print('Attach this image with 128 MiB RAM. The default build and old image are unchanged.')


if __name__ == '__main__':
    main()
