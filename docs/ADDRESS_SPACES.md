# Native address-space foundation

This document describes the shared address-space foundation and the private
BEX2 address spaces built on it. The BEX2 wire format is defined in
[BEX2_FORMAT.md](BEX2_FORMAT.md).

BEX1 files, ABI0–27, 64 KiB
contiguous offsets, 48 KiB executable cap, initial ESP65520 and synchronous exec
remain unchanged. Every syscall buffer now goes through one direction-aware
span helper. Input text, paths and file replacement bytes require readable user
pages; all result buffers, arguments including their NUL, and file-read outputs
require writable user pages. The replacement call validates both buffers before
any filesystem effect. Existing zero-length/pointer and full-capacity validation
rules remain per-call, including write's strict offset<extent and argument's
zero-capacity length query.

The pure span helper can inspect a 1024-entry sparse aperture table and checks
all covered pages with subtraction-safe bounds. The BEX1 compatibility path
retains its exact contiguous extent; there is no global increase in USER_CAPACITY.

## Named roots and lifetime

The existing static compatibility directory at 0xF00000 and user table at
0xF01000 retain their addresses and contents. A separate inactive kernel
directory at 0xF02000 is now explicitly reserved. PAGING_CAPACITY is 12 KiB;
owned-page eligible totals in the synthetic64/128/256 MiB profiles become
797/1053/33821. Every kernel-root entry maps supervisor-only identity memory.
The compatibility root still maps only the old64 KiB as user-accessible.

Initial paging selects the kernel root. Immediately before user entry the
compatibility root and BEX1 descriptors are installed. Immediately after
process_enter/process_resume returns, the named kernel root and BEX1 descriptor
context are restored, before FPU restoration, inactive publication, image copy,
or resource release. The C assembly boundary still restores kernel segments and
stack first. All root variants preserve supervisor identity mappings for kernel
code, stacks, devices and every eligible owned frame. The allocator continues to
exclude the entire16–20 MiB aperture, including aliases not currently mapped.
CR0.WP, CR4.PSE, x87 state preservation and ring3-only PIT suspension are retained.

No dispatch or mapping change occurs inside device polling or a syscall. No user
pointer survives suspension. No heap, allocation syscall, demand paging, COW,
swap, threads, new disk format or expanded app-security guarantee is introduced.

## Verification scope

The ordinary host span fixture inspects valid sparse table layouts with text,
data, workspace and top-stack regions, including adjacent virtual pages backed
by scattered frames. It never executes native bytecode or accesses guest memory.
Allocator tests inspect both actual static table constructions and every eligible
frame's supervisor mapping. The production lifecycle fixture spies on hardware
root/FPU transitions, checks root restoration before release, and retains real
owned-page allocation, copy, owner-lifetime and finite-capacity behavior.

These host checks do not replace guest testing of BEX1 and BEX2 programs.

## Private address spaces

`BASEOS_BEX2_ENABLED` in `src/program.h` is an explicit source gate for the
BEX2 loader and is set to 1. With it set to 0, process creation refuses BEX2
files as unsupported. The SDK defaults to BEX1, and no seeded app
is replaced or automatically converted.

A parser-validated BEX2 plan owns one directory, one user page table and exactly
its text/data/workspace/stack frames. The parser uses the fixed1024-owned-page
policy; creation separately checks live pool capacity for the complete footprint.
It then allocates zeroed PHYS_USER_IMAGE, PHYS_PAGE_DIRECTORY and PHYS_PAGE_TABLE
frames. All allocation and copy work completes before a process handle is
published. An ordinary capacity failure leaves output, other owners and pool
accounting unchanged. A later allocation failure rolls back only that new owner.
The header/null page, stack guard and undeclared gaps are absent from user mappings.
Rounded text/data page tails remain mapped and zero-filled. Only actual declared
text/data payload bytes are copied; BSS/workspace/stack start zero. Source file
offsets and application pointers remain relative offsets.

Every directory has supervisor identity mappings outside the one user aperture.
Its only user PDE references the owned PT; text entries are present/user/read-only,
data/workspace/stack are present/user/writable, other user entries are absent.
Data descriptor limit is encoded0x3FF with page granularity (effective0x3FFFFF);
code descriptor ends at rounded text end minus1 with byte granularity. Root and
descriptor transitions preserve and temporarily mask interrupt flags. Returning
from process_leave restores the named inactive kernel root before FPU context,
clearing active state, any copy or any physical release.

BEX2 slices switch mappings and do not gather/scatter an entire image. BEX1 still
uses its exact16-frame gather/scatter path. Both retain the existing x87 and
ring3-only PIT scheduling policy. Teardown checks every owned kind and clears
private lists only after inactive return. Legacy exec rejects BEX2 before entry;
a BEX1 application's arbitrary exit code remains an application result.

ABI1.1 adds only call28 and memory-info feature bit4. The old96-byte query remains
unchanged in size and preserves its reserved words. Bit5 is context-specific,
set only for an actually running BEX2 process. Its4MiB user_bytes value denotes
a sparse virtual extent; the separate128-byte memory query specifies valid
regions and current accounting. See [NATIVE_PLATFORM_ABI.md](NATIVE_PLATFORM_ABI.md).

The enabled host fixture runs real production process creation, table building,
owner allocation, span validation, dispatcher, scheduler and cleanup functions.
Only privileged transitions and physical/virtual-to-host buffer translations
are adapters. Valid linked apps cover initialized data, multi-page BSS,1MiB and
3MiB workspace declarations. It checks all syscall copy classes at data/workspace/
stack offsets, readonly-input acceptance, bounded outputs, cross-page reads,
query prefix/tail rules, mixed BEX1/legacy exec, owner-independent retained bytes,
ordinary finite capacity rejection and successful zeroed reuse after a close.
These host checks do not cover hardware mapping or guest persistence.
