"""Atomically update boot code without replacing filesystem sectors.

A legacy 1.44 MB image is extended in place logically, with unchanged LBAs.
Keep the old image as a unique .bak file before replacing any existing image.
Never run this against an image currently open by an emulator.
"""
import argparse
import fcntl
import os
import pathlib
import tempfile
from layout import constants


def update(image, boot, kernel):
    # QEMU uses POSIX byte-range locks. Hold a conflicting whole-file lock
    # until replacement is complete, so a running guest cannot lose writes
    # into the old inode. File contents remain untouched on lock failure.
    if image.exists():
        with image.open('r+b') as original:
            fcntl.lockf(original, fcntl.LOCK_EX | fcntl.LOCK_NB)
            _update(image, boot, kernel, original.read())
    else:
        _update(image, boot, kernel)


def kernel_offset(offset, c=None):
    """Map a byte offset in the linked kernel to its split disk extent."""
    c = c or constants()
    first = c['KERNEL_PRIMARY_SECTORS'] * c['SECTOR_SIZE']
    if not 0 <= offset < c['KERNEL_SECTORS'] * c['SECTOR_SIZE']:
        raise ValueError('kernel offset outside reservation')
    return (c['SECTOR_SIZE'] + offset if offset < first else
            c['KERNEL_EXT_LBA'] * c['SECTOR_SIZE'] + offset - first)


def install_kernel(data, kernel, c=None):
    """Install both code extents without touching either filesystem snapshot."""
    c = c or constants()
    sector = c['SECTOR_SIZE']
    first = c['KERNEL_PRIMARY_SECTORS'] * sector
    extra = (c['KERNEL_SECTORS'] - c['KERNEL_PRIMARY_SECTORS']) * sector
    tail = c['KERNEL_EXT_LBA'] * sector
    if (c['KERNEL_PRIMARY_SECTORS'] + 1 > c['FS_DISK_LBA'] or
        c['FS_SECOND_LBA'] < c['FS_DISK_LBA'] + c['FS_DISK_SECTORS'] or
        c['KERNEL_EXT_LBA'] < c['FS_SECOND_LBA'] + c['FS_DISK_SECTORS'] or
        tail + extra > c['DISK_SECTORS'] * sector):
        raise ValueError('invalid split kernel/filesystem layout')
    if not kernel or len(kernel) > first + extra:
        raise ValueError('kernel exceeds its loader reservation')
    if len(data) != c['DISK_SECTORS'] * sector:
        raise ValueError('disk has wrong size')
    data[sector:sector + first] = bytes(first)
    data[tail:tail + extra] = bytes(extra)
    data[sector:sector + min(len(kernel), first)] = kernel[:first]
    if len(kernel) > first:
        data[tail:tail + len(kernel) - first] = kernel[first:]


def _update(image, boot, kernel, old=None):
    c = constants()
    sector = c['SECTOR_SIZE']
    boot_data, kernel_data = boot.read_bytes(), kernel.read_bytes()
    if len(boot_data) != sector or boot_data[-2:] != b'\x55\xaa':
        raise ValueError('invalid boot sector')
    if not kernel_data or len(kernel_data) > c['KERNEL_SECTORS'] * sector:
        raise ValueError('kernel exceeds its loader reservation')
    if old is not None and len(old) not in (2880 * sector, c['DISK_SECTORS'] * sector):
        raise ValueError('unrecognized image size; refusing to overwrite it')
    data = bytearray(old or b'')
    data.extend(bytes(c['DISK_SECTORS'] * sector - len(data)))
    data[:sector] = boot_data
    install_kernel(data, kernel_data, c)
    image.parent.mkdir(parents=True, exist_ok=True)
    if old is not None:
        # One backup per distinct previous image, rather than unbounded copies
        # of identical data on no-op rebuilds. Never overwrite an older backup.
        import hashlib
        digest = hashlib.sha256(old).hexdigest()[:16]
        backup = image.with_name(image.name + '.' + digest + '.bak')
        if not backup.exists():
            with backup.open('xb') as f:
                f.write(old)
                f.flush()
                os.fsync(f.fileno())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=image.parent, delete=False) as f:
            temporary = pathlib.Path(f.name)
            fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
            os.replace(temporary, image)
            temporary = None
            directory = os.open(image.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=pathlib.Path)
    parser.add_argument('boot', type=pathlib.Path)
    parser.add_argument('kernel', type=pathlib.Path)
    args = parser.parse_args()
    try:
        update(args.image, args.boot, args.kernel)
    except (ValueError, OSError) as exc:
        parser.exit(1, f'image update failed: {exc}\n')
