# Bounded owned physical pages

This is a kernel foundation, not an application heap or BEX2 support. The boot
path initializes page metadata and prints its initial counts. Process backing uses it to back each
desktop process's fixed 64 KiB saved image with sixteen owned frames; see
[PROCESS_BACKING.md](PROCESS_BACKING.md) for its lifecycle.
There is no new syscall, app-visible physical address, page handle, disk format,
SDK ABI, image size, or change to the compatibility page tables and the separate inactive kernel root.

## Eligibility and fixed reservations

`src/physmem.c:STATIC_RESERVATIONS` is the allocator's explicit reservation
inventory. Each named entry uses the owning `src/layout.h` base/capacity rather
than a hand-maintained free-hole list. Entire arenas and their slack stay
reserved: low boot/kernel/stacks, software framebuffer, filesystem nodes,
Writer, Paint, published native canvases, disk/audio DMA, Sheet, desktop cache,
APPS and all its subarenas, compatibility page tables and the separate inactive kernel root, the whole 16–20 MiB user
aperture alias, drag/presentation/network/browser storage, both audio source
locations, image decoder, both filesystem profiles, TASK, editor and video.

The large audio source aliases the default filesystem pool/staging arena.
Default, large, fallback, and migrating mounts all retain exactly the same
reservation union. The allocator does not query filesystem or audio state.
Changing arena ownership or adding a new fixed arena requires reviewing this
inventory. Existing owners' size assertions and the inventory's range assertions
remain build-time guards; APPS/TASK slack is never reclaimed.

The dynamic framebuffer interval starts at the validated boot LFB and covers
the larger of boot pitch × height and four times `FB_CAPACITY`. It uses widened
arithmetic, rejects an end beyond the 32-bit physical address space, and rounds
both ends outwards to 4 KiB. It therefore covers subsequent supported 32-bit
display modes. A conflict with a usable E820 descriptor never makes it allocable.

The shared `platform_memory_map_available` predicate in `src/bootinfo.c` admits a
whole page only when enabled type-1 descriptors jointly cover it with no enabled
non-type-1 overlap. Adjacent, overlapping and unsorted usable ranges are allowed;
disabled and empty descriptors do not contribute. A zero/excessive count or an
enabled overflowing interval invalidates the map. No physical memory is probed.
The required-RAM admission checks in `platform_validate_memory` are unchanged.

The explicit initial ceiling is 256 MiB (65,536 PFNs). Before E820/LFB filtering,
the four holes below 127 MiB contain 797 pages:

| Start | Exclusive end | Pages |
|---|---|---:|
| `0x00510000` | `0x00600000` | 240 |
| `0x00680000` | `0x00700000` | 128 |
| `0x00F03000` | `0x01000000` | 253 |
| `0x01650000` | `0x01700000` | 176 |

Fully eligible pages in `[127 MiB, 256 MiB)` are additional candidates. Synthetic
fully usable PC-style 64/128/256 MiB maps therefore have 797/1,053/33,821 pages;
firmware reservations may reduce real guest counts. A RAM-size display estimate
is never used for eligibility. The kernel reserves the extra root explicitly at `KERNEL_DIRECTORY_BASE`; the total
`PAGING_CAPACITY` is 12 KiB. That removes one former free candidate page.

## Metadata and boot order

`TASK_PAGE_METADATA_BASE = 0x03090000` and capacity `0x50000` reserve:

- 65,536 aligned `uint32_t` owners (262,144 bytes)
- 65,536 one-byte kinds (65,536 bytes)

The exclusive end is `0x030E0000`, exactly the unchanged syscall-stack start.
Metadata dimensions, alignment, TASK containment, and validated RAM containment
are asserted by the production build. The process table must separately assert
its actual compiler-sized end is at or below `TASK_PAGE_METADATA_BASE`.
`tests/test_physmem.py` compiles the actual current `NativeTask` declaration with
`-m32` and verifies this boundary independently of the host pointer size.

An earlier compile measured `NativeTask` at 65,952
bytes, eight records at 527,616 bytes, ending at `0x03080D00`. This leaves 62,208
bytes before metadata. Do not substitute an older hand-calculated size for this
compiler result; rerun the check after integrating lifecycle/context changes.

`platform_init -> process_init` remains early descriptor initialization.
`kmain` calls `physmem_init` only after successful `platform_validate_memory` and
`video_init`. The wrapper independently checks the metadata destination's E820
coverage and video information before its first metadata write. Invalid setup
fails closed with an explicit boot diagnostic; zero eligible pages are an
ordinary successful initialization with zero capacity. Initialization never
zeroes candidate frames. A ready allocator cannot be reinitialized, including
when empty, so reset cannot discard ownership silently.

## Private lifecycle contract

All calls run in the existing serialized desktop/process context. IRQs and
device polling must not call this API. The busy guard rejects recursion, but is
not an SMP lock. Interrupts remain enabled during clearing. A future concurrent
caller needs an explicit synchronization design.

Owners are the nonzero, nonreused process-domain handles issued by `process.c`.
The allocator validates that domain; the process service is responsible for
issuing only live owners and never recycling their serials. There is no second
identity allocator. Kinds distinguish BEX1 backing, user image storage, page
tables and page directories; these names do not enable those future clients.

`physmem_alloc` accepts 1–1,024 frames per call, preflights capacity, claims
individually available pages without requiring contiguity, and clears every
page before publishing any output. Private transient claim bits distinguish
new pages from an owner's previous allocations of the same kind. The production
zero callback is infallible, does not allocate and clears exactly 4 KiB. Invalid
arguments or insufficient capacity leave metadata, counts and output unchanged.

`physmem_release` preflights the entire bounded list for alignment, bounds,
duplicates, owner and kind before changing anything. Wrong-owner/kind and
already-released lists return `PHYS_NOT_OWNED`; duplicate entries return
`PHYS_INVALID`. A stale former owner's list cannot free another owner's reused
frames. These private frame lists are not generation-tagged app capabilities:
the kernel caller must discard its list after release, particularly before the
same owner allocates again. `physmem_release_owner` returns all of one owner's
kinds and is idempotent. Clearing happens on allocation, not release.

Diagnostics expose eligible total, free, allocated, high-water, allocated count
per kind, and a per-owner count to kernel callers. They do not expose addresses
through the public application ABI. The high-water mark is lifetime-wide and
does not drop on release. Allocation counters always return to the eligible
baseline after cleanup.

## Verification scope

Run the focused ordinary suite with:

`python3 -m unittest discover -s tests -p test_physmem.py -v`

The host fixture links production `physmem.c` and `bootinfo.c` without altering
their logic. Its zero callback uses ordinary host buffers; guest addresses are
only integers. It covers normal profile maps, exact unions and reservations,
descriptor/page bounds, full-mode framebuffer exclusion, scattered allocations,
zero-before-publication, same-owner growth, mixed-kind cleanup, finite exhaustion,
unchanged capacity failures, ownership-checked release, and the maximum batch.

The existing `protect_memory` construction prefix runs against host tables, with
the privileged-register tail omitted. Inspection confirms all candidate frames
are supervisor identity-reachable and the entire user aperture stays excluded.
No guest address is accessed to test an absent mapping. The compile-only task
layout test and late-init source-order test complement the functional core.

Also retain existing boot-info, optional-memory and kernel-layout tests. These
host tests do not exercise a guest allocation/release lifecycle, performance or
native application capacity; see [PROCESS_BACKING.md](PROCESS_BACKING.md) for
the process-backing client. The boot-info sanitizer fixture may require
`ASAN_OPTIONS=detect_leaks=0`; the allocator fixture is an ordinary functional
host executable.
