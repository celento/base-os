"""BaseFS 2 images on the host (docs/BASEFS2.md), matching kernel/basefs.c.

Pure functions over bytes: probe, read the newest valid commit, and write a
new commit into blocks the current one does not use. Records are dicts with
parent (index, -1 for the root), flags, identity, revision, modified, data and
name (bytes).
"""
import struct
import zlib

BLOCK = 4096
SECTORS_PER_BLOCK = BLOCK // 512
RESERVED_BLOCKS = 3
SUPER_MAGIC = 0x32534642
CHECKPOINT_MAGIC = 0x43534642
FORMAT_VERSION = 1
SUPER_EXTENTS = 48
EXTENT_BLOCKS = 16
NAME_MAX = 255
MARKER_MAGIC = 0x44534F42
MARKER_VERSION = 3
ROOT_PARENT = 0xFFFFFFFF
DIRECTORY = 1
APP = 2
SUPER = struct.Struct('<12I96I19I')


class Corrupt(ValueError):
    pass


def marker(sectors):
    header = struct.pack('<6I', MARKER_MAGIC, MARKER_VERSION, sectors, 0, 0, 0)
    return (header + struct.pack('<I', zlib.crc32(header))).ljust(512, b'\0')


def probe(data):
    sectors = len(data) // 512
    return (len(data) % 512 == 0 and sectors >= (RESERVED_BLOCKS + 2) * SECTORS_PER_BLOCK
            and bytes(data[:512]) == marker(sectors))


def blocks_for(size):
    return (size + BLOCK - 1) // BLOCK


def read_super(data, slot):
    blocks = len(data) // BLOCK
    raw = bytes(data[(1 + slot) * BLOCK:(1 + slot) * BLOCK + 512])
    words = SUPER.unpack(raw[:508])
    (magic, version, block_size, count_blocks, low, high, next_identity, next_revision,
     cp_bytes, cp_crc, cp_extents, node_count) = words[:12]
    if (magic != SUPER_MAGIC or version != FORMAT_VERSION or block_size != BLOCK
            or struct.unpack_from('<I', raw, 508)[0] != zlib.crc32(raw[:508])
            or count_blocks != blocks or not 1 <= cp_extents <= SUPER_EXTENTS or not node_count):
        return None
    runs = [(words[12 + 2 * e], words[13 + 2 * e]) for e in range(cp_extents)]
    return dict(generation=low | high << 32, next_identity=next_identity,
                next_revision=next_revision, checkpoint_bytes=cp_bytes,
                checkpoint_crc=cp_crc, runs=runs, node_count=node_count)


def decode(payload, blocks, used):
    """Records of a checkpoint; marks every extent in used (a set)."""
    if len(payload) < 16:
        raise Corrupt('short checkpoint')
    magic, version, count, _ = struct.unpack_from('<4I', payload)
    if magic != CHECKPOINT_MAGIC or version != FORMAT_VERSION or not count or count > len(payload) // 36:
        raise Corrupt('bad checkpoint header')
    records, at = [], 16
    for i in range(count):
        if len(payload) - at < 36:
            raise Corrupt('short record')
        (parent, flags, identity, revision, modified, low, high,
         extent_count, length) = struct.unpack_from('<9I', payload, at)
        at += 36
        size = low | high << 32
        if (length > NAME_MAX or extent_count > (len(payload) - at) // 12
                or flags & ~(DIRECTORY | APP) or (flags & DIRECTORY and (size or extent_count))):
            raise Corrupt('bad record')
        extents, covered = [], 0
        for _ in range(extent_count):
            start, n, crc = struct.unpack_from('<3I', payload, at)
            at += 12
            if not 1 <= n <= EXTENT_BLOCKS or start < RESERVED_BLOCKS or start >= blocks or n > blocks - start:
                raise Corrupt('bad extent')
            for b in range(start, start + n):
                if b in used:
                    raise Corrupt('overlapping extents')
                used.add(b)
            extents.append((start, n, crc))
            covered += n
        if covered != blocks_for(size):
            raise Corrupt('extents do not cover the file')
        padded = (length + 3) & ~3
        if len(payload) - at < padded:
            raise Corrupt('short name')
        name = bytes(payload[at:at + length])
        at += padded
        if b'\0' in name or b'/' in name:
            raise Corrupt('bad name')
        if (parent != ROOT_PARENT or not flags & DIRECTORY or length) if i == 0 else \
                (parent >= count or parent == i or not length):
            raise Corrupt('bad tree')
        records.append(dict(parent=-1 if i == 0 else parent, flags=flags, identity=identity,
                            revision=revision, modified=modified, size=size,
                            extents=extents, name=name))
    for i in range(1, count):
        if not records[records[i]['parent']]['flags'] & DIRECTORY:
            raise Corrupt('parent is not a directory')
        walk, steps = i, 0
        while walk and steps <= count:
            walk, steps = records[walk]['parent'], steps + 1
        if walk:
            raise Corrupt('cycle')
    return records


def read_data(data, record):
    out = bytearray()
    for start, n, crc in record['extents']:
        chunk = bytes(data[start * BLOCK:(start + n) * BLOCK])
        valid = min(record['size'] - len(out), n * BLOCK)
        if zlib.crc32(chunk[:valid]) != crc:
            raise Corrupt('file data checksum')
        out += chunk[:valid]
    return bytes(out)


def mount(data):
    """(slot, super, records with data, used blocks) of the newest valid commit."""
    if not probe(data):
        raise ValueError('Not a BaseFS 2 volume')
    blocks = len(data) // BLOCK
    supers = [(s, read_super(data, s)) for s in (0, 1)]
    supers = sorted(((s, x) for s, x in supers if x), key=lambda p: -p[1]['generation'])
    for slot, sup in supers:
        try:
            payload = bytearray()
            for start, n in sup['runs']:
                if start < RESERVED_BLOCKS or start >= blocks or not n or n > blocks - start:
                    raise Corrupt('bad checkpoint run')
                payload += data[start * BLOCK:(start + n) * BLOCK]
            if len(payload) != blocks_for(sup['checkpoint_bytes']) * BLOCK:
                raise Corrupt('checkpoint length')
            payload = bytes(payload[:sup['checkpoint_bytes']])
            if zlib.crc32(payload) != sup['checkpoint_crc']:
                raise Corrupt('checkpoint checksum')
            used = set(range(RESERVED_BLOCKS))
            records = decode(payload, blocks, used)
            if len(records) != sup['node_count']:
                raise Corrupt('node count')
            for start, n in sup['runs']:
                for b in range(start, start + n):
                    if b in used:
                        raise Corrupt('checkpoint overlaps data')
                    used.add(b)
            for r in records:
                r['data'] = read_data(data, r)
            return slot, sup, records, used
        except Corrupt:
            continue
    raise ValueError('No valid BaseFS 2 commit')


def encode(records):
    out = bytearray(struct.pack('<4I', CHECKPOINT_MAGIC, FORMAT_VERSION, len(records), 0))
    for i, r in enumerate(records):
        name = r['name']
        size = len(r['data'])
        out += struct.pack('<9I', ROOT_PARENT if i == 0 else r['parent'], r['flags'], r['identity'],
                           r['revision'], r['modified'], size & 0xFFFFFFFF, size >> 32,
                           len(r['extents']), len(name))
        for start, n, crc in r['extents']:
            out += struct.pack('<3I', start, n, crc)
        out += name.ljust((len(name) + 3) & ~3, b'\0')
    return bytes(out)


class Allocator:
    def __init__(self, blocks, used, limit=None):
        self.limit = limit or blocks
        self.used = set(used)

    def run(self, want):
        block = RESERVED_BLOCKS
        while block < self.limit:
            if block in self.used:
                block += 1
                continue
            n = 0
            while n < want and block + n < self.limit and block + n not in self.used:
                n += 1
            self.used.update(range(block, block + n))
            return block, n
        raise ValueError('Volume full')


def write_files(image, allocator, records):
    """Places every record's data that has no extents yet."""
    for r in records:
        if r['flags'] & DIRECTORY or r.get('extents'):
            continue
        r['extents'], data, at = [], r['data'], 0
        while at < len(data):
            start, n = allocator.run(min(EXTENT_BLOCKS, blocks_for(len(data) - at)))
            chunk = data[at:at + n * BLOCK]
            image[start * BLOCK:(start + n) * BLOCK] = chunk.ljust(n * BLOCK, b'\0')
            r['extents'].append((start, n, zlib.crc32(chunk)))
            at += len(chunk)


def commit(image, records, *, slot, generation, next_identity, next_revision, allocator):
    """Writes a checkpoint and the superblock in slot (the one not current)."""
    payload = encode(records)
    decode(payload, len(image) // BLOCK, set(range(RESERVED_BLOCKS)))
    need, runs = blocks_for(len(payload)), []
    while sum(n for _, n in runs) < need:
        if len(runs) == SUPER_EXTENTS:
            raise ValueError('Volume too fragmented')
        runs.append(allocator.run(min(0xFFFF, need - sum(n for _, n in runs))))
    padded, at = payload.ljust(need * BLOCK, b'\0'), 0
    for start, n in runs:
        image[start * BLOCK:(start + n) * BLOCK] = padded[at:at + n * BLOCK]
        at += n * BLOCK
    flat = [w for run in runs for w in run] + [0] * (2 * (SUPER_EXTENTS - len(runs)))
    words = [SUPER_MAGIC, FORMAT_VERSION, BLOCK, len(image) // BLOCK, generation & 0xFFFFFFFF,
             generation >> 32, next_identity, next_revision, len(payload), zlib.crc32(payload),
             len(runs), len(records)] + flat + [0] * 19
    raw = SUPER.pack(*words)
    raw += struct.pack('<I', zlib.crc32(raw))
    image[(1 + slot) * BLOCK:(2 + slot) * BLOCK] = raw.ljust(BLOCK, b'\0')


def format_image(sectors, records=None, identity_seed=1):
    """A complete image: marker, records (default: an empty root) in slot 0."""
    image = bytearray(sectors * 512)
    image[:512] = marker(sectors)
    records = records or [dict(parent=-1, flags=DIRECTORY, identity=identity_seed, revision=0,
                               modified=0, data=b'', name=b'')]
    allocator = Allocator(len(image) // BLOCK, range(RESERVED_BLOCKS))
    for r in records:
        r['extents'] = []
    write_files(image, allocator, records)
    commit(image, records, slot=0, generation=1, allocator=allocator,
           next_identity=max(r['identity'] for r in records) + 1,
           next_revision=max(r['revision'] for r in records) + 1)
    return image


def update(data, records):
    """A new image with records committed after the current commit. Records
    whose data and flags match a current record of the same index and size
    keep its extents; the rest get new blocks."""
    slot, sup, current, used = mount(data)
    image = bytearray(data)
    for i, r in enumerate(records):
        old = current[i] if i < len(current) else None
        if old and old['data'] == r['data'] and old['flags'] == r['flags'] and old['extents']:
            r['extents'] = list(old['extents'])
        else:
            r['extents'] = []
    allocator = Allocator(len(image) // BLOCK, used)
    write_files(image, allocator, records)
    commit(image, records, slot=slot ^ 1, generation=sup['generation'] + 1, allocator=allocator,
           next_identity=max([sup['next_identity']] + [r['identity'] + 1 for r in records]),
           next_revision=max([sup['next_revision']] + [r['revision'] + 1 for r in records]))
    return image
