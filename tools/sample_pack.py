"""The sample pack that a new data disk carries until its first boot.

init_data.py --samples writes it into a blank marked data disk. The kernel
reads it once, on the boot that sets the disk up, and adds each file that is
not already there (src/kernel/fs.c, "Sample pack"). The first save then
overwrites the pack like any other free space.

Layout, all little-endian:
- The disk's last sector, outside both snapshot slots, is the header: magic
  "BSMP", version 1, first LBA, sector count, byte count, CRC32 of those bytes,
  file count, CRC32 of the preceding 28 bytes. The rest is zero.
- The pack sits directly below the header, inside the tail of the second
  snapshot slot. Each file is a u32 size, a u16 path length, a u16 zero, the
  path (folders separated by "/", no leading "/") and then the bytes.
"""
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SECTOR_SIZE = 512
MAGIC = 0x504D5342
VERSION = 1
NAME_MAX = 23  # names must also fit the snapshot volumes a new disk starts as
SAMPLES = ROOT / 'assets/samples'
# Shared with the release examples instead of keeping a second copy.
EXTRA = {'Media/Sample/Harbor theme.mp3': ROOT / 'assets/examples/harbor.mp3',
         'Media/Sample/Harbor sunset.mpg': ROOT / 'assets/examples/harbor.mpg'}


def default_files(root=SAMPLES):
    """Every file in the */Sample folders of assets/samples plus EXTRA,
    sorted by path. Other files there (README.md) are not samples."""
    files = {path.relative_to(root).as_posix(): path.read_bytes()
             for path in root.glob('*/Sample/*') if path.is_file()}
    files.update({name: path.read_bytes() for name, path in EXTRA.items()})
    return dict(sorted(files.items()))


def encode(files):
    out = bytearray()
    for name, data in files.items():
        parts = name.split('/')
        if any(not 0 < len(p.encode('ascii')) <= NAME_MAX or p in ('.', '..') for p in parts):
            raise ValueError(f'Invalid sample path: {name}')
        path = name.encode('ascii')
        out += struct.pack('<IHH', len(data), len(path), 0) + path + data
    return bytes(out)


def install(image, files, second_lba):
    """Write the pack into a blank image (bytearray). The second snapshot
    slot starts at second_lba; its first sector stays zero."""
    last = len(image) // SECTOR_SIZE - 1
    payload = encode(files)
    count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
    first = last - count
    if first <= second_lba:
        raise ValueError('Sample pack does not fit the second slot\'s tail')
    image[first * SECTOR_SIZE:first * SECTOR_SIZE + len(payload)] = payload
    header = struct.pack('<7I', MAGIC, VERSION, first, count, len(payload), zlib.crc32(payload), len(files))
    image[last * SECTOR_SIZE:] = (header + struct.pack('<I', zlib.crc32(header))).ljust(SECTOR_SIZE, b'\0')
    return first


def locate(data):
    """(first LBA, sectors) of a valid pack header in image bytes, or None."""
    if len(data) < 2 * SECTOR_SIZE or len(data) % SECTOR_SIZE:
        return None
    last = len(data) - SECTOR_SIZE
    header = data[last:last + 28]
    magic, version, first, count, size, _, _ = struct.unpack('<7I', header)
    crc = struct.unpack_from('<I', data, last + 28)[0]
    if magic != MAGIC or version != VERSION or crc != zlib.crc32(header):
        return None
    if not count or size > count * SECTOR_SIZE or first + count != last // SECTOR_SIZE:
        return None
    return first, count


def without_pack(data):
    """The image bytes with any pack and its header zeroed."""
    found = locate(data)
    if not found:
        return data
    first, count = found
    data = bytearray(data)
    data[first * SECTOR_SIZE:] = bytes(len(data) - first * SECTOR_SIZE)
    return bytes(data)


def decode(data):
    """{path: bytes} from the pack in image bytes; raises on damage."""
    found = locate(data)
    if not found:
        raise ValueError('No sample pack')
    first, count = found
    last = len(data) - SECTOR_SIZE
    _, _, _, _, size, crc, files = struct.unpack_from('<7I', data, last)
    payload = data[first * SECTOR_SIZE:first * SECTOR_SIZE + size]
    if zlib.crc32(payload) != crc:
        raise ValueError('Sample pack checksum mismatch')
    out, pos = {}, 0
    for _ in range(files):
        length, name_length, _ = struct.unpack_from('<IHH', payload, pos)
        pos += 8
        name = payload[pos:pos + name_length].decode('ascii')
        pos += name_length
        out[name] = payload[pos:pos + length]
        pos += length
    if pos != size:
        raise ValueError('Sample pack has trailing bytes')
    return out
