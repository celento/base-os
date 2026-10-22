# DOS programs

BaseOS runs DOS programs in DOSBox Staging 0.82.2, built as a BaseOS ELF
program against the SDL 2 port. DOSBox opens in a desktop window and takes
keyboard and mouse input. Its sound goes to the BaseOS audio stream, which
has not been tested with DOS programs yet.

## First-time setup

Build the cross toolchain and the ports once. Most of the time goes to
building GCC.

    python3 toolchain/build.py
    python3 ports/build.py

On the host you also need `mtools` and `mkfs.fat`: `brew install mtools
dosfstools` on macOS, or `sudo apt install mtools dosfstools` on Debian and
Ubuntu.

## Running a program

1. Put the DOS program in a folder named `dos-apps` in the repository, one
   subfolder per program, for example `dos-apps/WORD/`. Git ignores this
   folder. Use short names (8.3) for folders that DOS programs refer to.
2. Run `make run-dos`. This makes `build/baseos-dos.img` the first time,
   copies DOSBox and everything in `dos-apps` onto it, and starts QEMU with
   512 MiB and that disk attached.
3. In BaseOS, open Terminal and run `start /disk1/DOSBOX/DOSBOX`.
4. DOSBox mounts the disk's `DOS` folder as `C:`. Change into the program's
   folder and start it, for example `cd WORD` and then `WORD`.
5. Type `exit` at the DOS prompt, or close the window, to leave DOSBox.

Files the program saves go to the same disk and survive restarts. Running
`make run-dos` again adds new files from `dos-apps` but never overwrites a
file that is already on the disk. To start fresh, delete
`build/baseos-dos.img`. Use `DOS_APPS=path` and `DOS_IMG=path` to choose
other locations.

DOSBox reads its settings from `/DOSBOX/dosbox-staging.conf` on the disk
(`ports/dosbox/dosbox-staging.conf` in the source). Lines under
`[autoexec]` run at startup.

## Limits

- DOSBox uses its interpreting CPU core, so demanding games run slower than
  in DOSBox on a desktop OS.
- The build needs a CPU with SSE2, because DOSBox's resampler requires it.
- OpenGL shaders, MIDI synthesizers (FluidSynth, MT-32) and networking are
  not built in.
- You start DOSBox from Terminal. Files cannot open DOS programs yet.

`tools/dosbox_test.py` checks the whole path in QEMU with
`examples/dos/vgatest.asm`.
