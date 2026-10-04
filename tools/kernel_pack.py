"""Deterministic BaseOS packed boot image, preserving exact objcopy kernel bytes.

Disk bytes: 4096-byte plain bootstrap prefix, 32-byte BPK1 envelope, then one
complete raw or LZ4 block payload. CRC32 detects corruption, not authenticity.
This is an original implementation of the public LZ4 block format:
https://github.com/lz4/lz4/blob/v1.10.0/doc/lz4_Block_format.md
No LZ4 library or third-party implementation is copied or required at build time.
"""
import argparse
import pathlib
import struct
import zlib

from layout import constants

RAW = 0
LZ4 = 1
HEADER = struct.Struct('<8I')


def _length(output, length):
    while length >= 255:
        output.append(255)
        length -= 255
    output.append(length)


def compress_lz4(raw):
    """Greedy single-candidate LZ4 encoder with deterministic match selection.

    End with at least five literals; the last match starts at least twelve
    bytes before the end. These are the standard LZ4 block end conditions.
    """
    raw = bytes(raw)
    output = bytearray()
    previous = {}
    cursor = anchor = 0
    end = len(raw)
    while cursor <= end - 12:
        key = raw[cursor:cursor + 4]
        candidate = previous.get(key)
        previous[key] = cursor
        if candidate is None or cursor - candidate > 65535:
            cursor += 1
            continue
        length = 4
        while cursor + length < end - 5 and raw[candidate + length] == raw[cursor + length]:
            length += 1
        literals = cursor - anchor
        output.append(min(literals, 15) << 4 | min(length - 4, 15))
        if literals >= 15:
            _length(output, literals - 15)
        output.extend(raw[anchor:cursor])
        output.extend(struct.pack('<H', cursor - candidate))
        if length >= 19:
            _length(output, length - 19)
        # Index consumed positions too, allowing subsequent nearby matches.
        stop = cursor + length
        cursor += 1
        while cursor < stop:
            previous[raw[cursor:cursor + 4]] = cursor
            cursor += 1
        anchor = cursor
    literals = end - anchor
    output.append(min(literals, 15) << 4)
    if literals >= 15:
        _length(output, literals - 15)
    output.extend(raw[anchor:])
    return bytes(output)


def decompress_lz4(payload, raw_size):
    """Bounded reference decoder. Requires exact input and output consumption."""
    source = memoryview(payload)
    output = bytearray()
    cursor = 0

    def length(value):
        nonlocal cursor
        if value == 15:
            while True:
                if cursor >= len(source):
                    raise ValueError('truncated LZ4 length')
                extra = source[cursor]
                cursor += 1
                value += extra
                if value > raw_size:
                    raise ValueError('LZ4 length exceeds output reservation')
                if extra != 255:
                    break
        return value

    while cursor < len(source):
        token = source[cursor]
        cursor += 1
        literals = length(token >> 4)
        if literals > len(source) - cursor or literals > raw_size - len(output):
            raise ValueError('LZ4 literal exceeds input or output')
        output.extend(source[cursor:cursor + literals])
        cursor += literals
        if cursor == len(source):
            if len(output) != raw_size:
                raise ValueError('LZ4 output size mismatch')
            return bytes(output)
        if len(source) - cursor < 2:
            raise ValueError('truncated LZ4 offset')
        offset = source[cursor] | source[cursor + 1] << 8
        cursor += 2
        if not offset or offset > len(output):
            raise ValueError('LZ4 offset outside produced output')
        match = length(token & 15) + 4
        if match > raw_size - len(output):
            raise ValueError('LZ4 match exceeds output')
        for _ in range(match):
            output.append(output[-offset])
    raise ValueError('LZ4 block has no final literal sequence')


def pack_kernel(raw, c=None, codec=LZ4):
    """Pack canonical initialized bytes; enforce disk and linked-RAM budgets."""
    c = c or constants()
    raw = bytes(raw)
    if not raw or len(raw) > c['STACK_BOTTOM'] - c['KERNEL_LOAD_ADDR']:
        raise ValueError('raw kernel exceeds initialized RAM reservation')
    if codec == LZ4:
        payload = compress_lz4(raw)
    elif codec == RAW:
        payload = raw
    else:
        raise ValueError('unsupported kernel codec')
    header = struct.pack('<7I', c['KERNEL_PACK_MAGIC'], HEADER.size, codec,
                         len(payload), len(raw), zlib.crc32(raw), 0)
    header += struct.pack('<I', zlib.crc32(header))
    prefix = raw[:c['KERNEL_BOOTSTRAP_BYTES']].ljust(c['KERNEL_BOOTSTRAP_BYTES'], b'\0')
    result = prefix + header + payload
    if len(result) > c['KERNEL_SECTORS'] * c['SECTOR_SIZE']:
        raise ValueError('packed kernel exceeds boot loader reservation')
    return result


def unpack_kernel(packed, c=None):
    """Validate a packed artifact, returning the exact canonical raw image."""
    c = c or constants()
    prefix_size = c['KERNEL_BOOTSTRAP_BYTES']
    limit = c['KERNEL_SECTORS'] * c['SECTOR_SIZE']
    if not prefix_size + HEADER.size < len(packed) <= limit:
        raise ValueError('packed kernel size outside reservation')
    header = packed[prefix_size:prefix_size + HEADER.size]
    magic, size, codec, payload_size, raw_size, raw_crc, reserved, header_crc = HEADER.unpack(header)
    if (magic != c['KERNEL_PACK_MAGIC'] or size != HEADER.size or reserved or
            zlib.crc32(header[:28]) != header_crc):
        raise ValueError('invalid packed kernel envelope')
    if not 0 < raw_size <= c['STACK_BOTTOM'] - c['KERNEL_LOAD_ADDR']:
        raise ValueError('raw kernel exceeds initialized RAM reservation')
    payload = packed[prefix_size + HEADER.size:]
    if payload_size != len(payload):
        raise ValueError('packed payload size mismatch')
    if codec == RAW:
        raw = bytes(payload)
        if len(raw) != raw_size:
            raise ValueError('raw payload size mismatch')
    elif codec == LZ4:
        raw = decompress_lz4(payload, raw_size)
    else:
        raise ValueError('unsupported kernel codec')
    if zlib.crc32(raw) != raw_crc:
        raise ValueError('raw kernel checksum mismatch')
    if packed[:prefix_size] != raw[:prefix_size].ljust(prefix_size, b'\0'):
        raise ValueError('plain bootstrap differs from reconstructed kernel')
    return raw


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=pathlib.Path)
    parser.add_argument('packed', type=pathlib.Path)
    parser.add_argument('--codec', choices=('lz4', 'raw'), default='lz4')
    args = parser.parse_args()
    try:
        raw = args.raw.read_bytes()
        packed = pack_kernel(raw, codec=LZ4 if args.codec == 'lz4' else RAW)
        if unpack_kernel(packed) != raw:
            raise ValueError('packed kernel round-trip mismatch')
        args.packed.write_bytes(packed)
        print(f'Kernel: {len(raw)} initialized bytes -> {len(packed)} packed bytes')
    except (ValueError, OSError) as exc:
        parser.exit(1, f'kernel packing failed: {exc}\n')
