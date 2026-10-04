"""Create an explicitly marked, blank BaseOS 16 MiB data disk without overwriting.

python3 tools/init_data.py build/baseos-data.img

An existing data disk is only verified and is never modified. Boot BaseOS once
with this disk attached to initialize it or migrate a readable floppy snapshot.
Stop QEMU before running this tool, including for verification.
"""
import argparse
import fcntl
import os
from pathlib import Path
import tempfile
from volume import DATA_LAYOUT, SECTOR_SIZE, data_marker, disk_layout, locked_image, sync_directory


def verify(image):
    with locked_image(image) as source:
        if disk_layout(source.read()) != DATA_LAYOUT:
            raise ValueError('Existing image is not a marked BaseOS data disk; refusing to overwrite it')


def initialize(image):
    """Return True for a new disk or False for a byte-preserving verification."""
    image = Path(image)
    if image.exists():
        verify(image)
        return False
    image.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=image.parent, delete=False) as source:
            temporary = Path(source.name)
            fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
            source.write(data_marker())
            source.truncate(DATA_LAYOUT.sectors * SECTOR_SIZE)
            source.flush()
            os.fsync(source.fileno())
            try:
                # A same-directory link publishes complete bytes atomically and
                # cannot replace a path created while we were preparing them.
                os.link(temporary, image)
            except FileExistsError:
                verify(image)
                return False
            sync_directory(image.parent)
            return True
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    try:
        created = initialize(args.image)
    except (OSError, ValueError) as error:
        parser.exit(1, f'data image: {error}\n')
    if created:
        print(f'Created {args.image}: blank marked 16 MiB data disk. Boot BaseOS once before importing.')
    else:
        print(f'Verified {args.image}: existing data disk left unchanged.')


if __name__ == '__main__':
    main()
