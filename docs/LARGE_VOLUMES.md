# Optional large data volumes

The default machine remains **64 MiB RAM with a 16 MiB IDE data image**. Its
marker, snapshot version, capacities, boot-floppy layout and every application
address are unchanged. The optional profile uses **128 MiB RAM and an explicitly
created 64 MiB IDE image**. It is never selected just because more RAM exists.
No existing disk is automatically enlarged, reformatted or converted.

## Create or copy explicitly

Stop QEMU before using any image tool. To copy the latest saved files from an
existing IDE data disk into a **new** large disk:

```sh
python3 tools/migrate_volume.py build/baseos-data.img build/baseos-large-data.img --profile large
python3 tools/volume.py build/baseos-large-data.img info
```

The copy tool locks the source, reads its newest valid snapshot, checks the
selected destination capacity, preserves every node ID/name/content/parent,
application/directory flag and modification timestamp, and creates two verified
snapshots in the new image. It never writes source bytes. Publication is an
atomic no-replace link; an existing destination, including a symlink, is an
error. A locked source is rejected. Failure before publication leaves no
partially created destination. Keep the original disk as an independent backup.
Legacy v1/v2 snapshots have no timestamp field, so their imported timestamps
remain zero. Existing byte-valued names round-trip without Unicode replacement.

To create a blank marked image instead:

```sh
python3 tools/init_data.py build/baseos-large-data.img --profile large
```

The first guest boot can copy the **boot floppy's** readable files into a wholly
blank marked data disk. Those files may be older than an existing IDE disk, so
use the explicit copy tool above when upgrading an existing installation.
A blank marker alone does not contain a filesystem for host import/export;
boot it once or use the copy tool. No-argument `data_marker()` and
`initialize(path)` retain their exact default behavior and old marker bytes.
`initialize(path, profile='large')` only verifies an existing matching large
image, without changing any bytes; it refuses a default image at that path.

Attach the new image to the same legacy primary-master IDE device and set RAM
to 128 MiB. For example, after building the ordinary kernel and boot floppy:

```sh
qemu-system-i386 -m 128M -vga std -boot a \
  -drive file=build/baseos.img,format=raw,index=0,if=floppy \
  -drive file=build/baseos-large-data.img,format=raw,index=0,if=ide,cache=writeback
```

The ordinary `make run` target remains the default profile. The host exchange
tool automatically recognizes the exact supported marker/geometry and applies
that profile's file and node bounds. Imports retain QEMU-compatible locking,
content-addressed whole-image backups and atomic replacement. The optional
`--profile default` on the copy tool can create a new smaller destination only
if every file, node and aggregate byte count fits; it never truncates data.

## Geometry and limits

| Property | Default profile | Large profile |
| --- | ---: | ---: |
| Recommended guest RAM | 64 MiB | 128 MiB |
| Disk sectors, 512 bytes each | 32,768 | 131,072 |
| Marker version | 1 | 2 |
| Snapshot version | 4 | 5 |
| First / second snapshot LBA | 1 / 16,384 | 1 / 65,536 |
| Sectors per snapshot | 16,383 | 65,535 |
| Per-file bound, inclusive | 2,097,152 | 16,777,216 |
| Total node limit, including root | 256 | 256 |
| File bytes through 64 nodes | 8,385,024 | 33,550,848 |
| File bytes at 256 nodes | 8,377,344 | 33,543,168 |

The marker remains seven little-endian 32-bit words: `BOSD` magic, marker
version, disk sector count, snapshot sector count, two slot LBAs and IEEE CRC32
of the first 24 bytes. All remaining marker bytes must be zero. Only the two
known combinations of exact disk size and marker fields are recognized.

Version 5 retains the 28-byte checksummed header and 40-byte node records of
v4. Its independent version prevents older software from interpreting the
larger bounds as v4. The serialized payload allowance is
`(slot_sectors - 1) * 512`; file-data allowance subtracts
`40 * max(64, node_count)`. Additional node admission reserves that metadata
before mutating live state. The root, folders, apps and empty files all count.
Both formats retain one unused final disk sector. The legacy floppy's
v1/v2/v3 readers, v3 writer, 64 nodes and 16,383-byte file bound are unchanged.
Do not downgrade a large image to software that lacks marker v2/snapshot v5.

Snapshot saves retain the existing alternating-slot sequence: serialize and
validate, write payload, flush, read back and verify, then write/flush the
checksummed commit header and validate the complete new snapshot. A failed
preflight leaves prior data unchanged. The previous snapshot stays intact;
uncertain commit outcomes protect the disk until remount.

## Memory and API contract

The default file pool stays at `0x2000000..0x2800000` and default staging at
`0x2800000..0x3000000`. The selected large pool is
`0x3F00000..0x5F00000`, with staging at `0x5F00000..0x7F00000`.
All kernel, stack, app, native-task, editor and media addresses stay unchanged.
The large pool and staging each reserve 32 MiB. They are runtime addresses,
not large kernel BSS arrays.

Before reading or writing either high arena, the kernel calls
`platform_memory_range_available(base, end)` over the complete optional range.
It checks the actual BIOS E820 descriptors, joining adjacent usable ranges and
rejecting gaps, disabled descriptors, overflow and overlapping firmware
reservations. A rounded memory-size display is never used as proof.

A large disk attached to a 64 MiB machine is read only far enough to recognize
its one-sector marker, then left untouched. Boot-floppy files are exposed as a
protected recovery view. The storage warning explains the 128 MiB requirement;
no save reaches either disk. Unknown, unreadable or unrecognized optional disks
retain the same protected fallback behavior. Exact E820 coverage is required
even when a machine reports 128 MiB or more.

The selected arenas are separate from the temporary backend used during
migration. Seeded bytes are copied before their offset base changes; reading a
legacy floppy into the selected high pool does not reset those offsets to the
low pool. Alias handling uses the active pool and scratch ranges. Large
copies, CRC scans, blank scans and compaction service only the existing
non-mutating device background callback at bounded 4 KiB intervals.

- `fs_large_profile()` means the large IDE backend and its high arenas are
  currently selected. It is false for default IDE, floppy and recovery views.
- `fs_large_arenas_available()` means only that E820 says the high range is
  usable. It can be true while a default disk is actively using low arenas.
- `fs_file_limit()`, `fs_capacity()` and `fs_capacity_for_nodes()` report the
  selected backend's limits. `FS_FILE_MAX` remains the old 2 MiB constant for
  separately bounded consumers; `FS_LARGE_FILE_MAX` is additive.

Neither query alone grants an application memory ownership. Any later consumer
of the old 32–48 MiB arenas must check the active large profile **after
`fs_load_disk()`**, in addition to memory availability, and release ownership
before `fs_init()` or mount reconfiguration. `fs_init()` always seeds default
low storage. Mounted-large normal reads/writes/save and marker reads do not
borrow the old arenas; the marker uses a bounded kernel-stack buffer. Protected
fallback is conservative, even when migration happened to keep high staging.
This storage change does not raise audio, video, download, editor or native
program-image limits; each remains independently bounded.

## Verification

All inputs are generated disposable fixtures; saved user images are never used.
Run the focused host checks, including AddressSanitizer and UBSan:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_large_volume.py' -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_node_capacity.py' -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_fs_crc.py' -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_file_clipboard.py' -v
```

The new host suite covers explicit creation, stopped-source v1–v5 conversion,
source/destination/backup preservation, locks, exact maximum-sized files,
full 64/256-node capacities, alias/copy/delete/resize, atomic rejection and
metadata reclaim, low-RAM refusal and exact optional E820 queries. Python zlib
independently verifies both full 33,553,408-byte serialized payloads and their
headers. It also verifies that 128 MiB with a default disk retains low arenas,
and that mounted-large save/remount leaves the old arenas byte-exact.

```sh
make -j3 build/boot.bin build/kernel.bin
python3 tools/large_volume_test.py build --compile-only
python3 tools/large_volume_test.py build --keep
```

The guest runner prepares four kernels and runs nine serialized ordinary boots:
full write/reboot/save/read-only restart, protected low-RAM startup, floppy
migration/reboot, explicit v4 conversion, and the existing desktop/session
checks with an ordinary native app streaming a complete 16 MiB file in 4 KiB
chunks. It retains exact images and serial logs with `--keep`, on failure or in
compile-only mode. There are no intentional memory/CPU fault probes or fuzzing.

At the initial implementation checkpoint, the focused host checks and full
kernel/guest-fixture builds passed; the guest runner was prepared but had not
yet been run. See the subsequent verification record before claiming guest QA.
