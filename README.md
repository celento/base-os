# BaseOS

A small hobby operating system written from scratch in C and x86 assembly. It boots directly into a graphical desktop with persistent files, document editing, HTTP browsing, media playback, and small protected native applications.

BaseOS runs in QEMU with 64 MiB of RAM, or 128 MiB for the optional large-data profile. The desktop is rendered entirely in software using a 256-color backbuffer and antialiased Geist type. The look is Aqua: curved windows with pinstriped title bars and traffic-light controls, blue gel buttons and scrollbars, a File / Edit / Window menu strip under each title, a light glass shelf at the bottom with the BaseOS mark, a centered Launchpad, and a hand-drawn color icon set. Each of the three themes is a bundled wallpaper with its own accent. Settings switches between 800 × 600, 1024 × 768, 1280 × 720 and 1280 × 800, with a timed confirmation and automatic revert.

![Files and Editor running side by side in BaseOS](screenshots/gallery/desktop.png)

## What you can do

- Open up to eight windows. Move, resize, maximize, minimize or snap them; keep independent documents, folders and terminals open.
- Edit documents up to 65,535 bytes with selection, clipboard, eight undo steps, and case-sensitive or insensitive find/replace. Changed documents have Save/Discard/Cancel on close; saving confirms disk synchronization. Draw with Paint's brushes, shapes, fill and text tools.
- Calculate small budgets in Spreadsheet: a 128 x 26 grid, bounded formulas, number/currency/percent display formats, adjustable column widths, range copy/paste, four-step undo, native .bsh files, CSV value interchange and guarded recovery. Start with Documents/budget.bsh.
- Write formatted documents with Writer: proportional wrapping, bold/italic/underline, headings, alignment, grouped undo, native save/recovery and separate RTF or paginated Letter/A4 PDF exports. Writer supports 32,768 ASCII text bytes; the plain Editor remains available.
- IDE autosaves advance in bounded desktop turns while private document edits, playback and native apps continue. File mutations can report temporarily busy; Settings, Todo and completed downloads retain pending work. Writer/Spreadsheet IDE saves use owned completion; other legacy explicit saves can still block. See [background storage](docs/BACKGROUND_STORAGE.md).
- Store files up to 2 MiB on a separate 16 MiB data disk, or explicitly choose the large profile for 16 MiB files on a 64 MiB disk. Dual snapshots hold about 8 MiB or 32 MiB of file data respectively; both support 256 total file, folder and application nodes (64 in legacy floppy-only mode). Existing boot-disk files migrate without changing the old snapshots.
- Sort Files by Name, Type, Size or Modified, and filter the current folder with Ctrl+F. Each window keeps identity-safe selection through reordering.
- Copy and move files or folders through Files with Ctrl+C/Ctrl+X/Ctrl+V or the Edit menu. Collisions never overwrite unrelated files, and stale file identities are rejected.
- Read HTTP pages in Browser, follow links, navigate history, and save complete HTML for offline reading. Download binary files with Browser’s Download button or Terminal while other apps keep running.
- Play PCM WAV and MP3 through QEMU's SB16 device, with pause, resume, volume and a playlist. Play MPEG-1 program streams with synchronized MP2 audio; 48 kHz audio is resampled to 44.1 kHz for QEMU SB16. Exact bounds are in the media guides.
- View JPEG, PNG, BMP, GIF's first frame and BaseOS images, with fit, actual size, zoom, pan and transparency. Images are bounded to 1,024 pixels per side and 786,432 pixels total.
- Build C applications with the host SDK: unchanged BEX1 supports 48 KiB images in 64 KiB; opt-in BEX2 supports 256 KiB images and launch-declared private regions in a sparse 4 MiB extent, subject to actual free pages and the per-process limit. Native tasks can stream files in bounded chunks (2 MiB by default, 16 MiB on the large profile), replace 32 KiB documents with explicit sync, and use an optional 320x200 canvas. The included DocStats app accepts an optional document path, counts its bytes/words/lines and displays a byte histogram. The [ABI 1.3 file-transaction service](docs/NATIVE_FILE_TRANSACTIONS.md) adds conditional staged RAM publication up to 256 KiB with separately confirmed durability; UI remains 1.1, and Close/Stop remains forced.
- Native canvases publish stable frames at explicit present/yield/sleep/exit boundaries. Safe front-client updates avoid rebuilding the desktop; menus and overlapping/structural changes retain conservative full redraws.
- Launch `.bex` native apps directly from Files or `Ctrl+Space`. BEX1/unflagged BEX2 stay hosted in Terminal; explicitly GUI-flagged BEX2 apps open independent native windows with owned input and bounded Output. Try `/Programs/pointer-window.bex`. Run shell scripts, Tiny BASIC and the legacy synchronous `exec` interface. Calculator, Todo, Clock, Calendar and the original games remain included. System Monitor lists native tasks with Show and Stop controls, alongside resource and window information.
- Keep local Calendar appointments with optional times, day markers and guarded edits. Unfinished entries recover after a confirmed disk save or normal shutdown; see [Calendar](docs/CALENDAR.md).
- Search apps and files with `Ctrl+Space`; use the mouse wheel to scroll. Restore saved window arrangements and Editor/Paint/Writer/Spreadsheet drafts after reboot.

BaseOS is an educational custom BIOS/i386 OS, not a Linux distribution or a POSIX environment. Built-ins cooperate inside the kernel; native tasks have bounded execution slices and either the legacy 64 KiB region or BEX2 launch-declared private pages. Independent native Close is currently forced; heap/dynamic growth, keyboard-event streams, IPC and native networking remain open platform work. Browser is HTTP-only: no TLS/HTTPS, JavaScript, CSS layout, forms, authentication or embedded web images. There is one network request at a time. Real network tests use controlled QEMU host fixtures; public upstream DNS depends on the host network, so unrestricted internet access is not claimed. UEFI and general-purpose virtual-memory processes are not implemented.

Detailed guides: [Files](docs/FILES.md), [Spreadsheet](docs/SHEET.md), [Writer](docs/WRITER.md), [PDF export](docs/WRITER_PDF.md), [Browser](docs/BROWSER.md), [networking](docs/NETWORK.md), [downloads](docs/DOWNLOADS.md), [native SDK](docs/NATIVE_SDK.md), [image formats](docs/IMAGE_FORMATS.md), [Editor safety](docs/EDITOR_SAFETY.md), [task management](docs/SYSTEM_MONITOR.md), [audio](docs/MEDIA.md), [video](docs/VIDEO.md), and the [technical reference](docs/TECHNICAL.md).

## Screenshots

These captures come from the running QEMU guest with sample documents and artwork.

### Files

A sidebar of places (Home, Documents, Pictures, Media, Programs, Trash) with a storage meter, back, forward and up buttons, a clickable path, sortable columns, a name filter and color-coded file types.

![Files showing the Documents folder with the places sidebar and path bar](screenshots/gallery/files.png)

### Writer

Formatted documents with bold, italic and underline, headings, left/center/right alignment and proportional wrapping. Writer saves native `.bwr` files and exports RTF or paginated Letter/A4 PDF.

![Writer editing a formatted document with headings and inline styles](screenshots/gallery/writer.png)

### Spreadsheet

A 128 x 26 grid with formulas, number/currency/percent formats, adjustable column widths and CSV interchange. The sample `Documents/budget.bsh` totals a small monthly budget.

![Spreadsheet showing the sample budget with planned, actual and remaining columns](screenshots/gallery/spreadsheet.png)

### Browser

Read HTTP pages and local HTML files, follow links, save pages for offline reading and download files in the background. The built-in guide opens from `file:///Documents/welcome.html`.

![Browser displaying the local BaseOS guide page](screenshots/gallery/browser.png)

### Media Player and Image Viewer

Media Player plays WAV, MP3 and MPEG-1 video through QEMU's SB16 device with a playlist and volume control. Image Viewer opens JPEG, PNG, BMP, GIF and BaseOS images with fit, zoom and pan.

![Media Player listing a track next to Image Viewer showing a Paint picture](screenshots/gallery/media.png)

### Calendar

Monthly view with a day agenda. Appointments can be all-day or timed, days with entries are marked, and unfinished entries recover after a confirmed disk save or normal shutdown.

![Calendar showing a month view and three appointments for the selected day](screenshots/gallery/calendar.png)

### Paint

A 160 × 100 canvas with drawing tools, a color palette, undo/redo, and saved images that can be opened in Image Viewer.

![Paint displaying a pixel-art mountain landscape](screenshots/gallery/paint.png)

### Programming

Edit a numbered BASIC program and run it in Terminal. BASIC supports integer expressions, loops, keyboard input, and a small graphics canvas. Terminal also runs command scripts and protected BEX1 native programs.

![BASIC source in Editor and its generated color canvas in Terminal](screenshots/gallery/programming.png)

### Games

Wordle and 2048 running together. Snake, Mines, and Breakout are available from the same desktop.

![Wordle guesses alongside an active 2048 game](screenshots/gallery/games.png)

### Wallpapers and utilities

Three bundled wallpapers (the drawn Aqua blue, and the Lake and Clouds photos from Unsplash), decoded from JPEG at boot and dithered into the 256-color palette,, a screen saver, and everyday utilities. Clock reads the machine's real-time clock; System Monitor reports the OS's resource and window information.

![Appearance settings and the analog clock with the Lake wallpaper](screenshots/gallery/themes.png)

### Launchpad and window menus

Click Apps in the taskbar or press `Ctrl+Space`. Launchpad opens in the middle of the screen with a search field above a grid of every application; typing filters apps and files, the arrow keys and the mouse wheel move through the grid and Enter opens the selection. Its footer holds About, Settings, the screen saver and Shutdown. There is no global menu bar: each window carries its own File, Edit and Window menus under the title, and `F10` opens the front window's File menu.

![Launchpad with the application grid and search field](screenshots/gallery/search.png)

## Build and run

You need Python 3, NASM, QEMU, and an ELF-targeting GCC/binutils toolchain.

### macOS

```sh
brew install nasm qemu x86_64-elf-gcc x86_64-elf-binutils
make run
```

### Linux

On Debian or Ubuntu:

```sh
sudo apt install build-essential gcc-multilib nasm qemu-system-x86 qemu-system-gui python3
make run
```

`make run` builds `build/baseos.img`, a 2.88 MB boot floppy, and creates `build/baseos-data.img`, a separate 16 MiB IDE data disk, only if it does not already exist. QEMU starts with 64 MiB RAM. Use `make headless` for a serial-console run without a graphical window. The data volume holds up to 8,385,024 file bytes, with 2 MiB per file and up to 256 total file/folder/app nodes. Metadata for nodes beyond 64 slightly reduces the byte allowance; see the technical reference.

### In a web browser

`web/` runs the real BaseOS disk images in the browser with the [v86](https://github.com/copy/v86) WebAssembly x86 emulator. `web/build.sh` builds the kernel and disks from source, then stages a static site in `web/dist`:

```sh
npm --prefix web ci
bash web/build.sh
npm --prefix web run serve   # http://localhost:8080
```

`vercel.json` points Vercel at the same script, so each deploy rebuilds the OS from the deployed commit. Click the screen to capture the mouse and keyboard; Esc releases the pointer. Files changed in the browser stay in that tab and are lost on reload. Networking is unavailable because v86 does not emulate the RTL8139 adapter.

An [optional large-volume profile](docs/LARGE_VOLUMES.md) uses 128 MiB RAM and
an explicitly created 64 MiB data disk, with 16 MiB files and about 32 MiB of
file data. The default build and existing disks stay unchanged; migration is
a copy into a new image, never an in-place resize.

Use `make run-large` (or `make headless-large`) to create or verify the separate
`build/baseos-large-data.img` and boot with 128 MiB RAM. This does not copy an
existing IDE disk: follow the guide's explicit migration command first if you
want to keep that disk's current documents. WAV/MP3 can use up to 16 MiB of source
data in this profile; Editor, Writer, MPEG and download limits are unchanged.

At the first boot with a newly marked data disk, BaseOS reads the existing floppy files and saves a copy to the data disk. Both old floppy snapshots remain untouched. Later boots use the data disk. Without the optional IDE disk, the old floppy filesystem remains usable with its original limits. An unknown or unreadable IDE disk is never formatted automatically; BaseOS exposes the boot files read-only for recovery.

The build keeps the exact linked `kernel.bin` and derives a deterministic `kernel.packed` for the BIOS boot disk. Packing can accommodate initialized kernels above the former raw disk limit, subject to compressed disk capacity and the unchanged kernel/BSS RAM bound. See the [packed boot layout](docs/TECHNICAL.md).

Normal rebuilds preserve the filesystem area and back up an existing image before changing it. `make clean` keeps both disk images and backups. Shut QEMU down before rebuilding its disk or using the host file exchange tool. Use **System → Shutdown** to flush pending saves before closing the emulator.

Host and reference-media tests additionally need Clang, FFmpeg and Pillow (`python3-pil` on Debian). Independent PDF checks use pypdf, ReportLab, PyMuPDF and Poppler; missing optional readers are explicitly reported as skipped. None is a kernel runtime dependency. The [original Harbor examples](assets/examples/) provide 18 seconds of music and a nine-second animation. The normal build uses checked-in font data. Regenerating it with `python3 tools/gen_font.py` additionally requires Pillow.

## Start exploring

| Action | Shortcut or command |
| --- | --- |
| Search apps and files | `Ctrl+Space` |
| Switch windows | `Alt+Tab` |
| Snap left / right | `Alt+Left` / `Alt+Right` |
| Maximize / restore | `Alt+Enter` |
| Save Editor, Paint or a complete Browser page | `Ctrl+S` |
| Export Writer PDF / RTF | `Ctrl+Shift+P` / `Ctrl+Shift+E` |
| Filter Files / select sorting | `Ctrl+F` / `Ctrl+1` through `Ctrl+4` |
| Find / replace in Editor | `Ctrl+F` / `Ctrl+H` |
| Find next / previous | `F3` / `Shift+F3` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| List terminal commands | `help` |
| Read a command's manual | `man basic` |

Open `Programs/counter.bex`, `notebook.bex`, `docstats.bex` or `pointer.bex` from Files, or search
for their filenames with `Ctrl+Space`. Counter and Notebook explicitly synchronize
before showing Saved. Existing disks keep their installed example binaries; see
the SDK guide before deliberately replacing an older example.

Try the included examples in Terminal:

```text
run /Programs/demo.sh
basic /Programs/demo.bas
exec /Programs/hello.bex
start /Programs/counter.bex
```

The [desktop and programming guide](FEATURES.md) covers all shortcuts, persistence behavior, file exchange, BASIC syntax, and the native-program ABI. The [technical reference](docs/TECHNICAL.md) covers hardware assumptions, disk recovery, memory layout, rendering, and regression checks.

## Development

```sh
make
make test
python3 tools/regression.py build --extended
python3 tools/render_test.py build --optimized
```

Host tests use AddressSanitizer and UndefinedBehaviorSanitizer. If the host runs tests under ptrace, use `ASAN_OPTIONS=detect_leaks=0 make test`; this disables only unsupported LeakSanitizer, not the other sanitizers. QEMU checks use disposable disk images. Additional boot, storage, and process-isolation checks are documented in the technical reference.

The compositor caches the background during window drags and uploads changed screen regions. A controlled headless QEMU benchmark rendered 20 drag frames in 31 timer ticks, about 45 FPS. This measures guest rendering, not a guaranteed display frame rate on every host.

| Directory | Contents |
| --- | --- |
| `src/` | Bootloader, kernel, graphics, filesystem, apps, and interpreters |
| `tests/` | Host and QEMU regression fixtures |
| `tools/` | Disk tools, font generation, test runners, and screenshot capture |
| `examples/`, `sdk/` | Native assembly and C examples, application SDK |
| `assets/` | Fonts, licenses, cursor, and artwork |
| `screenshots/gallery/` | The README screenshots |
| `docs/` | Technical reference |
| `build/` | Generated binaries and persistent disk image, ignored by Git |

To regenerate the gallery, run `python3 tools/gallery.py build`. It links a documentation-only fixture that supplies sample app content, then captures the real guest through QEMU. It never boots or modifies your saved disk image.

## License

MIT. The bundled Geist and Geist Mono fonts have their own [OFL licenses](assets/fonts/), the toolbar glyphs come from Lucide ([ISC](third_party/lucide/)), and the application icons are BaseOS's own SVG drawings in [assets/icons](assets/icons/) (MIT, like the rest of the project). The Aqua wallpaper is drawn by `tools/gen_aqua_wallpaper.py`; the other wallpapers in [assets/wallpapers](assets/wallpapers/) are photos published on Unsplash under the [Unsplash License](https://unsplash.com/license), with each one's source recorded in `tools/gen_wallpapers.py`. Pinned third-party decoders retain their original licenses and notices: [minimp3](third_party/minimp3/), [stb_image](third_party/stb/) and [pl_mpeg](third_party/pl_mpeg/). Local patches and attribution are recorded alongside the vendored sources.

The [runtime and developer quick start](docs/RUNTIME_QUICK_START.md) summarizes the
runtime profiles, everyday applications and native platform limits.
