# Filesystem checksum performance

The filesystem uses a 256-entry, 1 KiB read-only lookup table for reflected
IEEE CRC32 (`0xEDB88320`). This replaces the previous eight bit iterations per
byte. The initial and final complements, exact bytes covered, v1 rolling
checksum reader, on-disk format, I/O ordering and snapshot commit protocol are
unchanged. Device-only background polling still happens after each complete
4,096-byte block, including the final block when the length is a multiple of
4,096. There is no initialization or additional writable state.

With GCC 14.2.0 and the production i386 `-Os` flags, `fs.o` text plus read-only
data grows from 12,415 to 13,428 bytes (+1,013). Writable data remains 4 bytes and
BSS remains 1,096 bytes. The complete initialized production kernel grows from
407,020 to 408,012 bytes (+992 after linker alignment), within the existing
491,008-byte disk reservation.

## Compatibility checks

Run the independent, sanitizer-enabled host tests:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_fs_crc.py' -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_node_capacity.py' -v
```

The CRC tests call the actual static filesystem implementation. Python's zlib
independently checks empty input, `123456789`, all 256 single bytes, patterned
inputs up to 8 MiB, and lengths around the 4 KiB polling boundaries. A separate
strong test callback verifies exactly `floor(length / 4096)` polls per call.
The persisted-image check creates a valid full 256-node volume, saves, remounts,
and saves again; zlib checks both complete 8,387,584-byte serialized payloads
and their headers. The host decoder also verifies exact file bytes, generations,
marker and reserved sector. The ordinary capacity suite covers v1/v2/v3/v4
compatibility, including the old full 64-node volume and expanded 256-node
volume. The native C harnesses run with ASan and UBSan enabled.

## Real-QEMU comparison

```sh
make -j3 build/boot.bin build/kernel.bin
python3 tools/fs_crc_benchmark.py build --baseline-ref <git-ref> --runs 3 --keep
```

`--baseline-ref` names the git revision whose `src/fs.c` is used as the
baseline (for example, the last revision with the bitwise CRC). The runner
compiles the baseline and current filesystem sources with identical
production flags and links them against the same ordinary build objects. It
uses QEMU 10.0.13 TCG, 64 MiB RAM, and a primary-master IDE disk with writeback
caching. Each boot gets new disposable boot and data images; saved `build/*.img`
files are never opened. It runs strictly serially, alternating pair order
before/after, after/before, before/after. Run without other emulators or heavy
host work for a useful comparison.

Each input disk contains two independently zlib-encoded valid full snapshots:
256 nodes and 8,377,344 file bytes (8,387,584 serialized payload bytes). The guest
measures initial mount, a full save after an ordinary same-length overwrite,
and remount, verifying every populated file after each phase. These use the
normal filesystem/device code. A test-only linker alias supplies a fixed file
modification timestamp so that full output images can be compared byte for
byte. This alias is absent from production builds. GUI/audio playback is not
started in this focused storage benchmark.

In a three-pair comparison, the lookup table reduced median full-save and
remount ticks by roughly 13% and initial mount by about 2%; host noise and
emulated I/O affect the result, so this is a measured workload result, not a
fixed speedup guarantee. One PIT tick is 1/70 second. All output disks were
byte-identical.

Both snapshots were independently decoded and CRC-checked; the previous slot,
boot floppy, data marker and final reserved sector were unchanged. `--keep`
retains the serial logs, exact images, compiled comparison binaries and
`results.json`, including compiler/QEMU versions, source hashes, common-object
hashes, raw tick/wall samples and medians. A failed run retains its files too.
No fault probes or malformed-volume tests are part of this benchmark.
