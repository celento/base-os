# Browser

BaseOS Browser is a small graphical HTTP and local-file reader written in C. It
uses the real RTL8139 / IPv4 / DNS / TCP / HTTP stack described in
[NETWORK.md](NETWORK.md). It is not a modern web engine.

## Controls

- Type an address and press Enter, or click Go. An omitted `http://` prefix is
  added; `host:port/path` also works.
- Back / Forward or Alt+Left / Alt+Right revisit the last 16 locations.
- Reload, Ctrl+R, or F5 fetches the current page again. Escape or Stop cancels
  only the browser's own request.
- Ctrl+L selects the address. The field supports ordinary insertion, selection
  of the whole address with Ctrl+A, Left/Right, Home/End, Backspace and Delete.
- Click an underlined link. Tab / Shift+Tab select links and Enter follows one.
- Mouse wheel, Up/Down, Page Up/Page Down, Space and Home/End scroll the page.
  The scrollbar track can also be clicked.
- Save or Ctrl+S writes a complete original page to a unique file in
  `/Downloads`; open the saved HTML/text file offline. See [DOWNLOADS.md](DOWNLOADS.md).
- Download or Ctrl+D saves the edited address, selected link, or current
  address to a unique `/Downloads` file. Type a binary URL with Ctrl+L and use
  Ctrl+D without first pressing Enter. To download a link with the mouse, click
  **Pick link**, click the link, then **Download**. Tab / Shift+Tab followed by
  Ctrl+D is the keyboard equivalent. Ordinary clicks and Enter still open links.
- **Progress** shows the latest download started by this Browser: destination,
  byte count, HTTP result and RAM/disk synchronization status. **Page** returns
  to the unchanged document. Both views support scrolling, including at the
  360 x 200 minimum. The extra controls deliberately occupy a third toolbar row.
- **Cancel** or Escape in the progress view cancels only Browser's active
  download. Closing Browser leaves the download running; reopen and click
  Progress to inspect it. No Browser action cancels another app's download.
- Home opens `about:home`. Opening Browser itself makes no network request.
- `file:///path` reads a local BaseOS file. `.html` and `.htm` are parsed as HTML;
  other local files are shown as text. The desktop can use `browser_open_file`
  to open a selected file directly.

## What is displayed

The bounded HTML reader handles titles, headings, paragraphs, bold text,
preformatted text, lists, basic table rows, common character entities, image
alternative text, named anchors and clickable links. Script and style contents
are omitted. Forms show an unsupported notice. Plain-text HTTP responses are
shown literally. UTF-8 is reduced to the available ASCII font repertoire, with
common typographic punctuation mapped to similar ASCII characters.

Relative links, root-relative links, query strings, fragments and HTTP redirects
are resolved. At most five redirects are followed. A redirect to HTTPS displays
a clear limitation message; it is never silently downgraded to HTTP. Non-HTTP
redirects are not followed. Browser can show a readable HTTP error response
alongside its actual HTTP status.

## Important limits

- HTTP is unencrypted. Do not use it for passwords or private information.
- There is no HTTPS / TLS, JavaScript, CSS layout, form submission, image
  decoding, cookie store, authentication, tabs, bookmarks or page
  cache. Many modern websites require these features and will not work.
- Download saves HTTP files up to the volume limit (2 MiB on the IDE data
  volume) using the separate background service. Save remains limited to the
  complete original 32 KB page source. Downloads never start automatically.
- The network stack permits one request at a time. If Terminal owns it, Browser
  reports that the network is busy rather than cancelling that request.
- URLs are limited to 255 characters. One 32 KB buffer holds the HTTP body,
  including its terminating zero. Larger responses are visibly marked as
  truncated.
- Parsed text is capped at 32 KB, with 96 links, 64 named anchors and 2,048
  display rows. History contains 16 locations in RAM and is not persisted.
- Rendering fits client areas from 360 x 200 upward. Resizing recomputes word
  wrapping. Built-in apps still run cooperatively, without separate processes.

## Kernel integration

`src/browser.h` is the public interface. The app is a singleton and initializes
lazily. The desktop owns its window, title bar, app menu, file associations and
input routing.

1. Reserve `BROWSER_BASE = 0x1610000`, `BROWSER_CAPACITY = 0x40000`, and validate
   RAM through `0x1650000`. The browser uses less than the reserved 256 KB and
   adds only four bytes of kernel BSS. Add `browser.c` to the kernel build.
2. Call `net_poll()` in the cooperative desktop loop, then `download_tick()` and `browser_tick()`.
   Mark the browser window dirty if the latter returns nonzero. Neither call
   waits for a complete HTTP request.
3. Draw with `browser_draw(client_x, client_y, client_w, client_h)` and route
   client clicks with the same geometry to `browser_click`.
4. Route scancode / ASCII events to `browser_key(sc, ch, modifiers)`. Modifier
   bits are `BROWSER_MOD_CTRL`, `BROWSER_MOD_SHIFT` and `BROWSER_MOD_ALT`.
   Browser must receive its Ctrl+L / Ctrl+R / Ctrl+S / Ctrl+D and Alt+Left / Alt+Right shortcuts
   before generic app or window shortcuts consume them.
5. `browser_scroll(lines)` scrolls down for a positive value. Closing the
   singleton calls `browser_close()`, which stops only its owned page request;
   a download already accepted by the background service continues.
6. `browser_open(url)` and `browser_open_file(fs_id)` navigate a source;
   `browser_title`, `browser_url`, `browser_status` and `browser_loading` expose
   its current page presentation state. The download panel has its own bounded
   presentation snapshot and never replaces the page body. These calls do not open a desktop window;
   the desktop must open/focus it first.

A request's `NetHttpResult.request_id` is checked before consuming or cancelling
it. If another app has already replaced a completed request, Browser explains
that the user should reload. Network response bytes remain in the caller-owned
body buffer. Old parsed content remains readable while a new request is pending.

## Verification

```sh
python3 -m unittest discover -s tests -p test_browser.py
make
python3 tools/browser_test.py build
python3 tools/browser_download_input_test.py build
```

The host test uses ASan/UBSan and real graphics code. It exercises HTML/text
parsing, URL resolution, asynchronous lifecycle, actual link hit testing,
address editing, history branching, reload and cancellation, request ownership,
HTTP-to-file progress/cancellation, mouse link picking, keyboard downloads,
query/fragment-safe names, collision suffixes, busy ownership, disk sync state,
local files, scrolling, resize reflow and pixel containment at four window
sizes. A sandbox that cannot run LeakSanitizer under its tracing layer can use
`ASAN_OPTIONS=detect_leaks=0`; address and undefined-behavior checks remain on.
Browser itself uses no heap allocations.

The QEMU fixture fetches real pages from a loopback HTTP server through the
emulated RTL8139 device. It checks typed navigation, a clicked relative link,
Back/Forward, Reload, Stop while a response is pending, input/drawing while
loading, HTTP and HTTPS redirects, local HTML and resize drawing. It writes a
serial log, request list and framebuffer screenshot in a printed temporary
directory. QEMU runs headlessly with a disposable disk and uses QMP pipes for
the screenshot. The normal saved disk is neither read nor modified.

This focused fixture verifies the browser and real network path. The separate
desktop integration tests must additionally cover launcher/menu wiring, window
routing, close/reopen behavior and emulated PS/2 input.
