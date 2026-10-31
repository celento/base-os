# DOS programs

BaseOS runs DOS programs in DOSBox Staging 0.82.2, built as a BaseOS ELF
program against the SDL 2 port. DOSBox opens in a desktop window and takes
keyboard and mouse input. Its sound goes to the BaseOS audio stream.

## Programs that come with BaseOS

These free programs are on the DOS disk and in /Applications. Open one from
the Launcher (type its name) or double-click it in Files.

| Program | What it is | License |
| --- | --- | --- |
| FED Editor | Text editor with menus, folding and syntax colors | GNU GPL 2 |
| NetHack | Dungeon exploration game, version 3.6.7 | NetHack General Public License |
| Floppy Bird | Fly through the gaps with the space bar | MIT |
| Vitetris | Falling-blocks puzzle game | BSD 2-clause |
| Sudoku86 | Sudoku with mouse and keyboard | BSD 2-clause |

They come from the FreeDOS package archive (gitlab.com/FreeDOS), pinned to
fixed commits in `tools/dos_apps.py`. On the disk each program has its own
folder under `/DOS` with its documentation and license, and
`/DOS/SOURCE` holds the source code of each one. `/DOS/BUNDLED.TXT` lists
where each program came from. FED and Vitetris use the CWSDPMI DPMI host,
which is freely redistributable and ships with its documentation.

## Setup

Build the cross toolchain and the ports once. Most of the time goes to
building GCC.

    python3 toolchain/build.py
    python3 ports/build.py

On the host you also need `git`, `mtools` and `mkfs.fat`: `brew install
mtools dosfstools` on macOS, or `sudo apt install mtools dosfstools` on
Debian and Ubuntu.

After that, `make run` does the rest. It makes `build/baseos-dos.img` the
first time, copies DOSBox and the bundled programs onto it (fetching them
the first time into `build/dos-apps-src`), and starts QEMU with 512 MiB and
that disk attached as `/disk1`. Before DOSBox is built, `make run` starts
BaseOS without the DOS disk and says so.

## How a launcher works

A `.dos` file holds one line: the path DOSBox starts, usually a batch file
on the DOS disk such as `/disk1/DOS/NETHACK.BAT`. Opening the file starts
`/disk1/DOSBOX/DOSBOX` with that path. DOSBox mounts the folder holding the
batch file as `C:`, runs it, and closes when it ends. A path to a `.EXE` or
`.COM` file works the same way. Each `.dos` file directly in
`/Applications` also shows as an app in the Launcher, named without `.dos`.

BaseOS creates the launchers for the bundled programs on every boot when
they are missing. To add your own, make a text file that ends in `.dos`
with the path in it.

## Adding your own programs

1. Put the DOS program in a folder named `dos-apps` in the repository, one
   subfolder per program, for example `dos-apps/WORD/`. Git ignores this
   folder. Use short names (8.3) for folders that DOS programs refer to.
2. Run `make run`. Everything in `dos-apps` is copied into `/DOS` on the
   DOS disk.
3. Make a launcher, for example `/Applications/Word.dos` holding
   `/disk1/DOS/WORD/WORD.EXE`, or start DOSBox from Terminal with
   `start /disk1/DOSBOX/DOSBOX`. Without a path DOSBox mounts `/DOS` as
   `C:` and waits at the prompt.
4. Type `exit` at the DOS prompt, close the window, or press Alt+F4 to leave
   DOSBox.

Files the programs save go to the same disk and survive restarts. Running
`make run` again adds new files but never overwrites a file that is already
on the disk. To start fresh, delete `build/baseos-dos.img`. Use
`DOS_APPS=path` and `DOS_IMG=path` to choose other locations.

DOSBox reads its settings from `/DOSBOX/dosbox-staging.conf` on the disk
(`ports/dosbox/dosbox-staging.conf` in the source). Lines under
`[autoexec]` run when DOSBox starts without a path.

## Limits

- DOSBox uses its interpreting CPU core, so demanding games run slower than
  in DOSBox on a desktop OS. It emulates fixed speeds, 3000 cycles per
  millisecond for real-mode programs and 60000 for protected-mode ones
  (`cpu_cycles` and `cpu_cycles_protected` in the settings file). On a slow
  host, such as QEMU without hardware acceleration, DOS time then runs
  slower than real time.
- The build needs a CPU with SSE2, because DOSBox's resampler requires it.
- OpenGL shaders, MIDI synthesizers (FluidSynth, MT-32) and networking are
  not built in.
- One DOS program can run per DOSBox window, and at most four run at once.
- Closing the window asks DOSBox to quit. A program that keeps DOSBox busy,
  such as Vitetris, may not react; closing it again after two seconds ends
  DOSBox. Quitting the program itself (Q in Vitetris) is the clean way.

`tools/dosbox_test.py` checks the whole path in QEMU with
`examples/dos/vgatest.asm`, and `tools/dos_apps_test.py` starts every bundled
program from the Launcher.
