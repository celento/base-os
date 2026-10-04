# Background downloads and offline pages

The original C/x86 BaseOS kernel can download an HTTP response directly to its
local filesystem while the desktop, Editor, native tasks, and audio keep running.
No Linux layer, helper VM, external download program, or remote write is used.

## Terminal

```
download http://10.0.2.2:8000/music.wav /Music/music.wav
downloads
cancel
```

The destination parent must already exist. Names containing spaces may be
quoted. `download` returns to the prompt immediately. `downloads` (or
`downloads status`) reports the latest transfer, byte count, HTTP status, file
limit, and disk save status. `cancel` or `downloads cancel` stops the transfer.
These commands are independent of the Terminal window: closing or reopening a
window does not cancel the job or deliver stale completion text to a new window.
Query `downloads` in any Terminal to see its result. Terminal command lines are
currently limited to 80 characters.

The IDE data volume permits a complete file of up to 2,097,152 bytes. When
running on the legacy floppy volume, the smaller filesystem file limit applies.
The one-transfer buffer is separate from Browser's 32 KB presentation buffer.
Only one network operation can run at a time. A busy Browser or another Terminal
operation is never cancelled to start a download, and download cancellation
checks the HTTP request ID before touching the network.

The complete response is kept in a bounded private arena. No destination file
exists while it is pending. Only complete HTTP 2xx responses can be committed;
network errors, incomplete responses, cancellation, size overflow, non-2xx
status, a changed destination folder, full storage, or exhausted file slots
leave no partial destination. An existing name is rejected at both start and
commit, so an unrelated file created while the download runs remains intact.
Writes are all-or-nothing in RAM. Normal filesystem autosave persists the result;
`downloads` explicitly distinguishes pending synchronization or disk errors.
Use System → Shutdown to flush saves before stopping the emulator.

## Browser Save

Click **Save** or press **Ctrl+S** after loading an HTML or text page. Browser
writes the original response bytes, including markup and omitted script/style
source, into `/Downloads/page.html` or `/Downloads/page.txt`. Repeated saves
choose `page-2`, `page-3`, and so on, without overwriting prior pages. Open that
file in Browser to read it offline. This saves one document only; linked pages,
images, stylesheets, and other resources are not downloaded.

The Save control is disabled while a request is loading or after a partial,
failed, unsupported, or stopped page. A truncated local file likewise cannot
be saved as if it were complete. Rendering limits alone do not prevent saving
an otherwise complete original source. Save results appear in Browser's status
bar. The built-in home/error documents are not saved as fetched page sources.

## Network limits

- HTTP only and unencrypted. Do not use it for passwords or private information.
- HTTPS is rejected; it is never downgraded.
- Downloads report redirects instead of following them. Use the final HTTP URL.
- Content-Length, chunked, and close-delimited HTTP body framing are supported.
- No compressed responses, cookies, credentials, authentication, uploads,
  resumable transfers, multiple concurrent downloads, or background queue.
- Every network request retains the existing 15-second total deadline. A slow
  server can therefore fail before reaching the 2 MiB file limit.
- Public DNS/HTTP access is unverified in the development cloud. Tests use only
  a loopback HTTP fixture, through the real QEMU RTL8139/user-network path.

## Integration

`src/download.h` exposes the singleton `download_start(cwd, url, path)`,
`download_tick()`, `download_cancel()`, `download_active()`, `download_status()`,
and `download_last_error()` APIs. Initialize lazily or call `download_init()`.
A zero start result means accepted; minus one means rejected, with a reason in
`download_last_error()`. The active job is preserved by a rejected start.

Call `download_tick()` immediately after `net_poll()` in the normal desktop
loop. It returns nonzero when visible status or files change. Never call it from
an IRQ, `platform_poll()`, or while retaining borrowed `fs_data()` pointers: a
completed transfer can mutate/compact filesystem storage. The file commit is
serialized with ordinary app actions and does not perform a synchronous disk
flush. The arena is `DOWNLOAD_BASE = 0xB30000`, capacity `0x210000`.

`browser_can_save()` reports whether a complete original page is available.
`browser_save_page(cwd, path)` saves to an explicit, non-existing path and returns
the new file ID or minus one. Browser's normal Save button chooses the unique
`/Downloads` destination. Route Ctrl+S to Browser before generic Editor/desktop
save handling consumes it.

## Verification

```
ASAN_OPTIONS=detect_leaks=0 make test
python3 tools/download_test.py build
```

Host ASan/UBSan tests combine the real HTTP parser and filesystem with a controlled
cooperative network fixture. They cover exact 2 MiB binary data, sync/remount,
Terminal responsiveness and quoted paths, cancellation, request ownership,
existing-file preservation, destination identity/rename changes, HTTP failures,
size limits, full volume/slots, and the smaller legacy volume limit. Browser
tests verify original byte preservation, unique Save destinations, incomplete
page rejection, reboot, Ctrl+S, actual Save hit testing, and client-area bounds.

The QEMU test creates disposable floppy and 16 MiB data disks. It downloads
20,037-byte and exact 2 MiB binary fixtures through RTL8139, checks cancellation
partway through a real response, oversized/incomplete/redirect/error responses,
existing-file preservation, changed folders, and offline Browser Save/reopen.
During the throttled 2 MiB response it edits/saves a real Editor document, runs
and saves a native counter task, plays actual SB16 audio, and redraws the desktop.
A second QEMU boot and host volume decoding verify exact saved bytes. Evidence
includes serial logs, packet captures, HTTP request paths, captured audio,
a framebuffer screenshot, and a SHA-256 digest. No normal persistent image is
read or modified.

### Production desktop input regression

`python3 tools/download_input_test.py build` boots the unmodified production
kernel twice with disposable disks and drives its launcher, Terminal, and
Browser through QMP-generated PS/2 keystrokes. It checks visible download
progress and cancellation, an exact 2 MiB transfer surviving Terminal
close/reopen, existing-file rejection, actual Browser Ctrl+S routing, unique
page names, and reopening the saved page after reboot without an HTTP request.
QMP memory observations are read-only; no app entry points or private kernel
functions are invoked. The host independently decodes the saved volume and
compares every binary and HTML byte. The printed evidence directories contain
screenshots, serial logs, the disposable data image, and verification hashes.
