# BaseFS 2

BaseFS 2 is the block-based format for the native BaseOS volume. It replaces
the snapshot formats (v4/v5), which hold the whole volume in memory and limit
the node count, name length and file size. In BaseFS 2 the node table is the
only thing loaded at mount; file contents stay on disk until read.

This page covers the on-disk format and the core in `kernel/basefs.c`. The
`kernel/fs.c` backend and the one-time migration of old volumes come in later
steps.

## Goals

- No fixed node count, name length (up to 255 bytes) or file size limit other
  than the disk.
- A crash or power cut at any moment leaves the volume at the last completed
  commit.
- Every byte read back is checked: file data per extent, the node table and
  the superblocks as a whole.

## Layout

Blocks are 4 KiB (8 sectors).

| Block | Contents |
| --- | --- |
| 0 | Data-disk marker in its first sector (`DATA_MARKER_MAGIC`, version 3, the sector count, CRC in word 6) |
| 1 | Superblock slot 0 |
| 2 | Superblock slot 1 |
| 3 and up | Checkpoints and file data |

A superblock is one sector: magic `BFS2`, format version, block size and
count, a 64-bit generation, the next node identity and revision, the
checkpoint's byte length, CRC and node count, and up to 48 extents (start,
length in blocks) where the checkpoint lives. A CRC32 of the preceding bytes
ends it.

A checkpoint is the complete node table: a 16-byte header (magic `BFSC`,
version, node count) then one record per node. A record has the parent's
index, flags (directory, app), identity, revision, modification time, 64-bit
size, extent count and name length (36 bytes), then its extents (start,
blocks, CRC32 of the bytes that belong to the file, 12 bytes each) and the
name padded to 4 bytes. Record 0 is the root. A file's extents hold at most
16 blocks (64 KiB) each, so a read checks at most 64 KiB more than it needs.

All numbers are little-endian.

## Commits

New data goes only to blocks that the current commit does not use and that
no write since has taken. A commit then:

1. checks the new table the same way mount will, and refuses it otherwise;
2. writes the checkpoint to free blocks, flushes, and reads it back;
3. writes the superblock with the next generation to the slot not in use,
   flushes, and reads it back.

Only after step 3 do the old commit's blocks become free. Mount reads both
slots, tries the newest valid generation first and falls back to the other
one when its checkpoint is damaged. If the superblock write fails, the
checkpoint's blocks stay reserved until the next commit or mount, because
the superblock may have reached the disk anyway.

The checkpoint may be split into at most 48 runs. Each run is as long as the
free space at that point allows, so only a heavily fragmented volume would
hit that; the commit then reports no space and the previous commit stays
current.

## Errors

| Code | Meaning |
| --- | --- |
| `BFS_ERR_IO` | The device failed a read, write or flush |
| `BFS_ERR_CORRUPT` | A CRC or structure check failed; data is never returned unchecked |
| `BFS_ERR_NOSPACE` | Not enough free blocks; nothing is half-written |
| `BFS_ERR_NOMEM` | Kernel heap exhausted |
| `BFS_ERR_FORMAT` | The disk has no BaseFS 2 marker |
| `BFS_ERR_INVALID` | Bad arguments, or a table mount would reject |

## Tests

`tests/test_basefs.py` builds `tests/basefs_host.c` with AddressSanitizer
and runs the core on an 8 MiB RAM disk. It formats and mounts, writes files
of 0, 1, 4096, 4097, 65536, 70000 bytes and 1 MiB and reads them back whole
and across extent boundaries, rewrites everything 20 times and checks that
free space returns to the same value, and checks that bad tables are refused.
It then cuts power at every sector write of a commit (including its data
writes) and requires every mount afterwards to show exactly the old or the
new state. It also flips bits in an extent (the read reports corruption) and
in the newest checkpoint (mount falls back one generation), fills the volume
and checks that every heap object comes back.
