# FAT volumes

`kernel/fat.c` reads and writes FAT12, FAT16 and FAT32 volumes, so disks and
disk images can move between BaseOS and other systems (DOS, Windows, Linux,
emulators). BaseFS 2 stays the native volume format; FAT is for exchange.

This page covers what the core supports, how it keeps a volume repairable
after a crash, how programs reach FAT disks, and how it is tested.

## What it handles

- Volumes that start at sector 0 (floppies, "superfloppy" images) or the first
  FAT partition of an MBR partition table (types 0x01, 0x04, 0x06, 0x0B, 0x0C,
  0x0E). The FAT type comes from the cluster count, as the specification says,
  not from the label in the boot sector.
- 512-byte sectors only.
- Long file names (VFAT) up to 255 UTF-16 units, read and written. Names are
  UTF-8 in the API. Short-only names written by DOS are read as code page 437.
- The Windows NT case flags: `readme.txt` is stored as a short entry with
  lower-case flags instead of a long name, as Windows does.
- Generated short names follow the specification's basis-name rules with a
  `~number` tail (`LONGFI~1.HTM`, up to `~65535`).
- Lookups ignore ASCII case and also match the short name.
- Files up to 4 GiB - 1 byte. Directories up to 65,536 entries. The FAT12/16
  root directory has the fixed size set at format time.
- The FAT32 FSInfo free count and next-free hint, kept up to date by
  `fat_sync`. When a volume was not cleanly unmounted, the free count is
  recounted instead of trusted.
- FAT32 volumes with FAT mirroring turned off (only the active FAT is used).

It does not format volumes, check or repair them, or handle sector sizes other
than 512 bytes, exFAT, or GPT partition tables.

## Crash behaviour

FAT has no journal. The core orders its writes so that a crash leaves a volume
a disk checker can repair without losing data that a finished call wrote:

- New clusters are marked in the FAT before the directory entry points at
  them. The FAT is written back to every copy before any directory entry.
- Data is written before the entry's size grows to cover it.
- When a file shrinks or is deleted, its entry changes first and the clusters
  are freed after. A crash in between leaves lost clusters, never an entry
  that points at a free cluster.
- A rename writes the new entry before erasing the old one. A crash in between
  leaves the file under both names (sharing its clusters) rather than under
  none; a checker resolves it.
- If the old name of a rename cannot be erased, the new name is taken back.
  If that fails too, the volume stops accepting changes until it is mounted
  again, so two names never stay live over one chain.
- If a moved folder's ".." entry cannot be updated, the volume likewise
  stops accepting changes, since later moves rely on that link to refuse
  putting a folder inside itself.
- Overwriting existing bytes in place is not atomic: a crash can leave a mix of
  old and new bytes in the range being written.

The volume is marked in use in FAT[1] (FAT16 and FAT32) at its first change and
marked clean by `fat_sync`, so other systems know to check it after a crash.
`was_dirty` reports a volume that another system did not unmount cleanly.

## Memory and caching

A mounted volume holds three buffers: an 8-sector window of the FAT (written
back to all FAT copies before anything that depends on it), one directory
sector, and one scratch sector. Each `FatNode` remembers the last cluster it
reached, so sequential reads and writes do not walk the chain from the start.
Copies of a node are independent: after one copy changes the file, other
copies are stale.

## FAT disks for programs

Exchange disks sit on the secondary ATA channel: the master is `/disk1` and
the slave is `/disk2` (`kernel/fatvol.c`). The primary master stays the native
BaseFS 2 volume. At boot each secondary disk is probed and the FAT volume on it
(whole disk or first MBR partition) is mounted; the serial log says
`FAT: mounted /disk1`, adding "(not cleanly unmounted)" when another system
left it marked in use.

ELF programs use them through the ordinary file calls (`kernel/ufile.c`):
open, read, write, seek, ftruncate, fsync, stat, fstat, readdir, mkdir,
unlink, rmdir, rename, chdir and getcwd. Details:

- Paths are normalized by name ("." and ".."), so `cd /disk1/GAMES; cd ../..`
  lands in the native root. `/` lists the mounted disks next to the native
  folders; a mounted disk hides a native root folder of the same name.
- Every open of one file shares one node, so all descriptors see the same
  size and data, and a rename through a path keeps open files working.
- Deleting an open file fails with EBUSY, as on other systems that use FAT.
- Renaming between volumes fails with EXDEV. Renaming onto an existing file
  replaces it, as POSIX rename does.
- Reads and writes go to the disk as they happen, in pieces of up to 64 KiB
  through a kernel buffer. FSInfo and the clean mark are written a second
  after the last change, or at once by fsync.
- The two secondary units share one channel and take turns; each request
  selects its unit before reading status, so probing one unit cannot confuse
  the other.

## Tests

`tests/test_fat.py` builds `tests/fat_host.c` (the core on an in-memory disk,
with ASan and UBSan) and checks it against dosfstools and mtools:

- files, long names and nested folders that `mcopy` wrote read back exactly,
  on FAT12, FAT16 and FAT32, and on a partitioned disk;
- random scripts of writes, truncates, folder creation, deletes and renames on
  each FAT type match a model, read back the same through `mcopy`, and pass
  `fsck.fat -n`;
- names: short, case-flagged, long, accented and generated aliases as `mdir`
  shows them;
- a full volume and a full FAT12 root directory refuse new data cleanly;
- a power cut at many points of a script leaves every chain consistent (only
  the renamed file may appear under two names), untouched files intact, and a
  volume `fsck.fat` repairs to clean.

`tools/fat_volume_test.py` boots the ELF fixture with a FAT32 disk made by
`mkfs.fat` and `mcopy` on the secondary master. `examples/elf/fatdisk.c` reads
a host file, makes folders, writes, patches, truncates and reads back a 3 MiB
file through two descriptors, appends, renames across folders and checks the
EBUSY, EXDEV and ENOTEMPTY cases. The host then runs `fsck.fat -n` and
compares what `mcopy` extracts.
