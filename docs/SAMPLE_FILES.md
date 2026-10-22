# Sample files

A data disk created by `make` (`tools/init_data.py --samples`) carries a set of
sample files. On the boot that sets the disk up, BaseOS adds them to
`Documents/Sample`, `Pictures/Sample` and `Media/Sample`, so every built-in app
has something to open. The list is in `assets/samples/README.md`.

The files never enter the kernel image. They are stored on the data disk
itself, in space the first save overwrites.

## Sample pack

`tools/sample_pack.py` writes the pack into a new, blank marked disk:

- The disk's last sector is the header: magic `BSMP` (0x504D5342), version 1,
  first LBA, sector count, byte count, CRC32 of those bytes, file count, and a
  CRC32 of the preceding 28 bytes. All numbers are little-endian 32-bit.
- The pack lies directly below the header, in the tail of the second snapshot
  slot. The first sector of that slot stays zero. Each file is a 32-bit size, a
  16-bit path length, a 16-bit zero, the path (for example
  `Pictures/Sample/Tiles.gif`, no leading slash) and the bytes. Each path
  component is 1 to 23 ASCII bytes, which fits the snapshot volumes a new disk
  starts as.

The data marker in sector 0 is unchanged, so the disk is still a blank marked
disk to every reader.

## First boot

`fs_load_disk` (src/kernel/fs.c) reads the header before it checks a snapshot
disk. While a valid pack lies wholly inside the second slot after its first
sector, the blank check ignores it. When the disk turns out blank and the files
come from the built-in defaults (not from a boot floppy volume being migrated),
the kernel reads the pack into the staging arena, checks its CRC, creates the
folders that are missing and adds each file that does not exist yet. The disk
is then formatted as BaseFS 2 and the first commit saves the samples with
everything else. Later boots mount BaseFS 2 and never look at the pack again,
so deleted samples stay deleted.

A damaged pack is skipped with `FS sample pack unreadable; skipped` on the
serial log; the disk is set up normally. Disks made without `--samples`, and
disks that already exist, are unchanged. To get the samples on an existing
setup, move `build/baseos-data.img` aside and run `make` again.

## Tests

- `tests/test_sample_pack.py`: pack layout, damage detection, name limits and
  `init_data.py --samples`.
- `tools/sample_files_test.py build`: boots default and large disks in QEMU and
  checks every sample byte for byte, plus a damaged pack and a disk without one.
