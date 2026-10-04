# Native address-space foundation: C3a

BEX2 loading is not enabled by this landing. BEX1 files, ABI0–27, 64 KiB
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

Host proof does not replace the staged BEX1 guest gates. Keep BEX2 disabled until
those gates and the later parser, private-table ownership, mixed-format and
capacity gates pass in the separately assigned normal runner.

C3a implementation verification (2026-10-04): 31 selected ordinary host tests
passed across address_space, physmem, process_lifetime, process_bindings,
native_platform, native_publication, native_publication_process,
native_arguments, native_launch, native_titles, native_render, native_rectangle,
native_files, native_sync and kernel_layout. The i386 kernel ELF and packed image
built with no warnings; initialized bytes506412, packed342551. NativeTask remains
508 bytes, table end0x03000FE0. No QEMU was run by this implementation task.
