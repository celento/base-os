"""Create an explicitly marked, blank BaseOS data disk without overwriting.

python3 tools/init_data.py build/baseos-data.img
python3 tools/init_data.py build/baseos-large-data.img --profile large

An existing data disk is only verified and is never modified. Boot BaseOS once
with this disk attached to initialize it or migrate a readable floppy snapshot.
Stop QEMU before running this tool, including for verification.
"""
import argparse
import fcntl
import os
from pathlib import Path
import tempfile
from volume import SECTOR_SIZE, data_layout, data_marker, disk_layout, locked_image, sync_directory


def verify(image, *, profile='default'):
    with locked_image(image) as source:
        if disk_layout(source.read()) != data_layout(profile):
            raise ValueError('Existing image does not match the requested BaseOS data profile; refusing to overwrite it')


def initialize(image, *, profile='default'):
    """Return True for a new disk or False for a byte-preserving verification."""
    layout = data_layout(profile)
    image = Path(image)
    if image.exists():
        verify(image, profile=profile)
        return False
    image.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=image.parent, delete=False) as source:
            temporary = Path(source.name)
            fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
            source.write(data_marker(profile))
            source.truncate(layout.sectors * SECTOR_SIZE)
            source.flush()
            os.fsync(source.fileno())
            try:
                # A same-directory link publishes complete bytes atomically and
                # cannot replace a path created while we were preparing them.
                os.link(temporary, image)
            except FileExistsError:
                verify(image, profile=profile)
                return False
            sync_directory(image.parent)
            return True
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--profile', choices=('default', 'large'), default='default')
    args = parser.parse_args()
    try:
        created = initialize(args.image, profile=args.profile)
    except (OSError, ValueError) as error:
        parser.exit(1, f'data image: {error}\n')
    if created:
        print(f'Created {args.image}: blank marked {data_layout(args.profile).sectors * SECTOR_SIZE // 1048576} MiB data disk. Boot BaseOS once before importing.')
    else:
        print(f'Verified {args.image}: existing data disk left unchanged.')


if __name__ == '__main__':
    main()
