# BEX2 v1 format and producer

The original producer landing supplied a pure parser/planner and host-tested
example algorithms without enabling a loader. The later private-space loader
now passed independent and combined default/large guest gates; see
[C3b verification](C3B_VERIFICATION.md) and [combined acceptance](C3_COMBINED_VERIFICATION.md).
BEX1 remains the default producer and unchanged binary contract. Fresh release
images may explicitly include the two workspace examples; existing saved disks
and kernel startup seeding are unchanged. This document defines the format, not
a claim of heap allocation or general dynamic virtual-memory services.

## Wire header

The first 64 bytes contain sixteen little-endian unsigned 32-bit words:

| Byte | Field | v1 meaning |
|---:|---|---|
| 0 | magic | `0x32584542` (`BEX2`) |
| 4 | header_bytes | 64 |
| 8 | format_version | 1 |
| 12 | flags | 0 for hosted; bit 0 requires one owned native window |
| 16 | file_bytes | Exact actual file length, inclusive cap 262,144 bytes |
| 20 | entry_offset | Inside actual text bytes |
| 24 | text_bytes | Nonzero text plus read-only data, excluding rounded tail |
| 28 | data_offset | `align_up(4096 + text_bytes, 4096)` |
| 32 | data_file_bytes | Initialized data bytes, including any linker alignment |
| 36 | data_mem_bytes | Initialized data, BSS and their linker padding |
| 40 | workspace_bytes | Page multiple; zero is valid |
| 44 | stack_bytes | Page multiple, 16,384 through 262,144 inclusive |
| 48 | required_abi_major | Must equal the loader's supported major |
| 52 | required_abi_minor | Must not exceed the loader's supported minor |
| 56 | reserved0 | 0 |
| 60 | reserved1 | 0 |

Shared definitions are in `sdk/baseos_executable.h`. Definitions alone do not
establish kernel support. The opt-in producer currently requests ABI 1.1 by
default for unflagged hosted output, independently of current SDK declarations.
`--window native-v1` sets required flag 1 and requires ABI minor at least 2;
unknown required bits or unavailable launch backends are refused, never silently
downgraded. See [native windows](NATIVE_WINDOWS.md).

`file_bytes == data_offset + data_file_bytes`; the header length must equal the
available input file length. Text starts at both file and virtual offset 4096.
Initialized data starts at both file and virtual `data_offset`. There is no
compact segment packing, relocation, dynamic linker, TLS or constructor phase.
The builder also rejects unsupported allocated ELF sections rather than silently
omitting them from the header. Empty initialized data leaves an explicit zero-
padded file ending at `data_offset`.

## Offset layout and commitment

All pointers remain offsets relative to the existing user segment base. Normal
C code is linked at these offsets, never at the kernel's linear USER_BASE.

- Virtual extent: 4,194,304 bytes. Offset `[0,4096)` is absent; neither the header
  nor its padding is copied into a user mapping.
- Text/rodata: `[4096,data_offset)`, rounded to pages and user read-only. Copy only
  `text_bytes`; zero the remaining tail before exposure.
- Data: from `data_offset`, committed through
  `workspace_start = align_up(data_offset + data_mem_bytes,4096)`. Copy only
  `data_file_bytes`; zero BSS, padding and rounded tail.
- Workspace: `[workspace_start,workspace_start+workspace_bytes)`, committed,
  zeroed, user writable. It is local app-managed storage, not a heap syscall.
- Stack: `[4194304-stack_bytes,4194304)`, zeroed, user writable. Initial ESP is
  4,194,288. Exactly one absent guard page immediately precedes the stack.
- Data/workspace must end at or before the guard. All remaining gap pages stay
  absent. Empty data and workspace consume zero pages. Text always consumes at
  least one page; stack consumes at least four.

The plan gives a byte-granularity code segment limit of rounded text end minus
one, and a data segment byte limit of 4,194,303. The latter requires the loader's
correct page-granularity descriptor encoding; these integers are not raw x86
limit fields. Read-only text does not establish NX, comprehensive W^X or a claim
that arbitrary untrusted programs are safe.

## Pure planning contract

`executable_plan_bex2(image, bytes, policy, out)` in `src/executable.c` reads no
live ABI, page allocator, filesystem or paging state and allocates nothing.
Input may be byte-unaligned. The caller supplies exact available input length,
supported ABI major/minor, mounted-backend file cap and fixed policy owned
page cap. Both caps are inclusive; zero means no capacity, not unlimited.
The format's 256 KiB file cap applies even if the filesystem supports more.

Every sum and page alignment is widened before narrowing. All declared ranges,
entry, format fields, overlap and capacity are checked before the successful
plan is assigned. Every failure leaves the output unchanged. Successful region
`bytes` are rounded committed bytes; `text_file_bytes` and `data_file_bytes` are
the separate exact copy lengths. Source file offsets equal region offsets.
The planner does not retain pointers. The caller must keep validated input
stable until payload copy completes, then allocate/copy/activate atomically or
roll back without disturbing existing owners.

Results:

- `EXECUTABLE_OK`: complete plan
- `EXECUTABLE_FORMAT`: missing/truncated arguments, wrong magic, inconsistent
  file/entry/data lengths or offsets, or non-page workspace/stack alignment
- `EXECUTABLE_UNSUPPORTED`: unknown header size/version/flags/reserved fields,
  or unsupported required ABI
- `EXECUTABLE_CAPACITY`: format/caller file cap, stack size policy, extent/guard
  overlap, or caller owned-page cap exceeded

Mapped page counts include text, data, workspace and stack. Owned pages include
those pages plus exactly two page-table overhead pages (one PD and one PT).
An admissible plan checks fixed policy; it is not a live-capacity snapshot,
reservation or activation. The loader separately checks current free pages.
Maximum virtual commitment can reach 1,022 user pages plus two table pages, but
available physical pages on a particular machine usually impose a lower limit.
The parser does not replace allocator admission or owner-checked rollback.

## Building and inspecting

Default commands and `build(source, output)` remain byte-identical BEX1 producers.
BEX2-specific options require explicit format selection:

```sh
python3 tools/build_app.py examples/c/workspace_array.c build/workspace-array.bex \
  --format bex2 --elf-output build/workspace-array.elf
python3 tools/build_app.py examples/c/workspace_index.c build/workspace-index.bex \
  --format bex2 --workspace-bytes 0x100000 --stack-bytes 65536
nm -n build/workspace-array.elf
```

Unflagged defaults are 1 MiB workspace, 64 KiB stack and required ABI minor 1.
Add `--window native-v1` only with BEX2 to select the owned-window contract; its
default required ABI minor is 2 and a lower explicit minor is rejected.
`--workspace-bytes 0` permits no workspace. Optional `--required-abi-minor` sets
a more specific build requirement. The builder checks generated header/ELF
agreement before publishing the BEX2 file; rejected capacity or unsupported
features preserve any previous output. `--elf-output` is BEX2-only.

`sdk/app2.ld` and `sdk/start2.c` are separate from BEX1. Their symbols include
`__text_start/end`, `__data_start`, `__data_file_end`, `__data_memory_end`,
`__workspace_start/end`, `__stack_bottom/top` and `__stack_guard`.
`sdk/baseos_app2.h` supplies linker-workspace accessors without changing
`baseos.h`, `baseos_abi.h` or existing syscall transfer caps.

## Examples and host evidence

Both opt-in examples consume their whole declared workspace while yielding.
They read the startup document (or `/Documents/stats-sample.txt`) through an
owned versioned handle, one 4 KiB chunk per call, and save a small conditional
report followed by an owned async synchronization operation. They check and
release handles. Reports are `/Documents/workspace-array-N.txt` and
`/Documents/workspace-index-N.txt`, where N is the legacy display/view slot (a Terminal for these hosted examples).

- `workspace_array.c` fills/checks every workspace word using a document-derived
  seed and records its unsigned checksum.
- `workspace_index.c` fills/checks the entire workspace and stores line offsets
  for the opened revision, failing normally if the declared index is full.

At the tested toolchain, both have 8,192-byte files, one text page, no data pages,
256 workspace pages and 16 stack pages: 273 mapped / 275 owned pages each.
At 3 MiB workspace each instead plans 785 mapped / 787 owned pages. Those are
layout calculations and host checks, not observed guest capacity. Two such
3 MiB apps require the separately selected 256 MiB profile gate.

The stable 56,812-byte stats fixture produces these host algorithm results:

| Example | Workspace | Result |
|---|---:|---|
| Array | 1 MiB | 262,144 words; seed 4,140,131,180; checksum 3,120,168,960 |
| Array | 3 MiB | 786,432 words; same seed; checksum 2,579,365,888 |
| Line index | 1 or 3 MiB | 904 lines; offset checksum 25,618,244 |

Focused commands:

```sh
PATH=/workspace/shared/baseos-tools/bin:$PATH \
TMPDIR=/workspace/shared/baseos-test-tmp ASAN_OPTIONS=detect_leaks=0 \
python3 -m unittest discover -s tests -p test_executable.py -v
```

The eight tests compile the production parser with ASan/UBSan; cover fixed
fields, exact and excessive ordinary capacity, large BSS, optional empty
regions, 16/64/256 KiB stacks, maximum virtual commitment, and ELF-symbol/header
agreement. They compile and run both real example algorithms against a bounded
host service model at 1 and 3 MiB, including normal busy creation/replacement retries and a sync wait.
They rebuild all five BEX1 examples and compare every byte and SHA-256 with the
frozen `tests/fixtures/bex1-hour05` files. The parser also compiles as freestanding
i386 without undefined runtime helper symbols.

No guest app is executed by these tests. No guest persistence, GUI behavior, mixed-format scheduling or performance
claim follows from those parser/toolchain tests. The separate C3 integration
adds the loader, root switching, memory query and owned allocation, gated and
verified as described in [ADDRESS_SPACES.md](ADDRESS_SPACES.md).
