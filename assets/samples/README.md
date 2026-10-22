# Sample files

New data disks made by `make` carry these files and the first boot puts them
in `Documents/Sample`, `Pictures/Sample` and `Media/Sample`:

| Folder | File | Opens in |
| --- | --- | --- |
| Documents/Sample | Welcome letter.bwr | Writer |
| | Trip notes.txt | Editor |
| | Trip budget.bsh | Spreadsheet (formulas and currency formats) |
| | Reading log.csv | Spreadsheet |
| | Pancakes.html | Browser |
| Pictures/Sample | Mountain lake.jpg, Sunset dunes.png, Tiles.gif | Image Viewer |
| | Robot.pbm | Image Viewer, Paint (160 x 100 BOS1) |
| Media/Sample | Doorbell.wav | Media Player |
| | Harbor theme.mp3, Harbor sunset.mpg | Media Player (from `assets/examples`) |

Everything here was drawn, written or synthesized by
`tools/make_sample_files.py` (Python and Pillow); nothing is downloaded and the
book titles and amounts are made up. MIT license, like the rest of BaseOS. The
Harbor files are described in `assets/examples/README.md`.

Regenerate with `python3 tools/make_sample_files.py`. How the files reach the
disk is in `docs/SAMPLE_FILES.md`.
