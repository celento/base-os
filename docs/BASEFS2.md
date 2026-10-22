# BaseFS 2

BaseFS 2 is the block-based format for the native BaseOS volume. It replaces
the snapshot formats (v4/v5), which hold the whole volume in memory and limit
the node count, name length and file size. In BaseFS 2 the node table is the
only thing loaded at mount; file contents stay on disk until read.

This page covers the on-disk format, the core in `kernel/basefs.c`, and how
`kernel/fs.c` uses it for the data disk, including the one-time conversion of
old volumes.

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

`bfs_commit` does all of this with blocking device calls. `bfs_commit_begin`
and `bfs_commit_end` split it so a caller can drive the disk itself in small
steps: begin validates, encodes and reserves the checkpoint blocks without
I/O, the caller writes and checks the blocks, and end takes the outcome (done,
failed before the superblock write, or uncertain after it).

Only after step 3 do the old commit's blocks become free. Mount reads both
slots, tries the newest valid generation first and falls back to the other
one when its checkpoint is damaged. If the superblock write fails, the
checkpoint's blocks stay reserved until the next commit or mount, because
the superblock may have reached the disk anyway.

The checkpoint may be split into at most 48 runs. Each run is as long as the
free space at that point allows, so only a heavily fragmented volume would
hit that; the commit then reports no space and the previous commit stays
current.

## Converting a disk in place

`bfs_create` sets up an empty volume in memory without touching sector 0
and with allocation limited to the blocks below a given limit. A converter
writes the files and the first commit there, then writes the BaseFS 2 marker
last. Until that final sector write the disk still carries its old marker
and its old contents above the limit, so a crash leaves the old volume as it
was.

## In the kernel

`kernel/fs.c` uses BaseFS 2 as data backend 3. A marked blank data disk (the
v1 or v2 marker that `tools/init_data.py` writes) is formatted as BaseFS 2 on
first boot, and a BaseFS 2 disk of any size mounts. Mount reads only the node
table.

### Nodes and names

On BaseFS 2 a name can be up to 255 bytes (`fs_name_max()`), and there is no
fixed number of nodes. The node table starts in the 64 KiB metadata arena and
moves to the kernel heap when it needs more slots, and each name is its own
heap allocation. What bounds the count is memory and the checkpoint: a node
is refused when the next checkpoint would no longer fit the staging arena
(8 MiB, or 32 MiB with the large arenas), which is on the order of a hundred
thousand nodes. Paths stay within `FS_PATH_LEN` (1,536 bytes) and 63 levels.
The snapshot formats keep their limits of 256 nodes (64 on the floppy) and 23
bytes per name.

### Files load on demand

The RAM file pool (8 MiB, or 32 MiB with the large arenas) is a cache of the
volume. A file's bytes are read the first time `fs_data` asks for them, with
each extent's CRC checked, and stay until the pool needs the room. Then clean
files leave least recently used first; the pool compacts, so a pointer from
`fs_data` is good only until the next mutation or the next `fs_data` call for
another file. Changed files always stay in memory until a save writes them.

Changed files may fill the pool only up to room for one more file of the
largest allowed size, so any file can still be loaded. A write that would go
past that saves first and then proceeds. A read that needs the disk while a
save is running finishes the save first, since the two cannot share the
device. If a file cannot be read, it reads as zeros at its size and is read
again the next time it has to load; it is not marked changed, so nothing is
written over the good copy on disk.

Each node remembers its extents and whether it changed since the last commit.
A save writes only changed files, then the checkpoint and a superblock, so
changing one small file costs a few blocks however full the volume is. The
save runs in the background sync job like the snapshot formats did: the job
plans the commit, writes and reads back each run of at most eight sectors,
compares, and then writes, flushes and checks the superblock. Files stay
locked while it runs. A save that fails before the superblock is written
returns its blocks, and the next save tries again.

A changed file is written to new blocks while its old copy stays allocated
until the commit lands, so the kernel admits a change only while the next
commit still fits in the free blocks: every changed file rounded up to whole
blocks, plus a checkpoint for the node count. A refused write returns an
error and leaves the file as it was.

The capacity shown to programs is the bytes in use plus what a new file could
still take, so it grows with the disk (reported up to 2 GiB). Room to rewrite
the largest file is held back from it, so a full volume can still save a
change to any one file; when the next save would not fit, a write saves the
pending changes first, which frees their old copies. On the 16 MiB default
disk with 2 MiB files that leaves about 14 MiB, against 8,385,024 bytes in
the snapshot format. Growth counts in whole blocks: a file can grow into the
rest of its last block on a full volume, and a write that needs a new block
is refused. A new node is refused once its record would eat into the held-back
room. A deleted file's blocks count as free once the delete is saved.

### Conversion of snapshot disks

A data disk in the v4 or v5 snapshot format converts to BaseFS 2 when it is
mounted, once:

1. If the current snapshot is in the second slot, the volume is saved to the
   first slot with the normal snapshot commit, so everything from the second
   slot onward is free.
2. `bfs_create` sets up an empty volume limited to the blocks below the second
   slot, every file is written there and committed, and the BaseFS 2 marker is
   written last.
3. The new volume is mounted again from disk and every file is read back with
   its CRCs checked. If any read or check fails, the old marker is written back and the
   snapshot is loaded again, so the disk stays in the snapshot format and the
   next boot tries again.

A power cut before the marker write leaves the snapshot disk as it was, and a
cut after it leaves a complete BaseFS 2 volume. Floppy volumes keep the
snapshot format. `FS_CONVERT_TO_BASEFS` (on by default) turns conversion off
for host tests of the snapshot formats.

## Host tools

`tools/basefs.py` reads and writes the same format in Python, and
`tools/volume.py` uses it for BaseFS 2 images (`info`, `ls`, `import`,
`export`, `mkdir`). A host commit goes to the superblock slot not in use and
keeps the extents of unchanged files.

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
in the newest checkpoint (mount falls back one generation), fills the volume,
converts a disk in place below a block limit, and checks that every heap
object comes back. It also cross-checks `tools/basefs.py`: the C code mounts
images the host wrote and the host mounts images the C code wrote.

`tests/fs_basefs_host.c` runs `kernel/fs.c` on BaseFS 2: formatting a blank
marked disk, a one-file save done in bounded background steps, repeated
rewrites of a 1 MiB file, a power cut at every third write of a save,
conversion from either snapshot slot, a power cut at every seventh write of
a conversion, a failed read at each point of a conversion (the old marker
comes back), and a disk of an odd size. `tools/data_volume_test.py` checks
the same in QEMU on real disk images, including a v4 disk with a folder that
converts on first boot and can be read by the host tools afterwards.
