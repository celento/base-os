"""Generate an original formatted quick-start document in BaseOS Writer v1."""
import argparse
import pathlib
import struct

PARAGRAPHS = [
    ('Writer quick start', 5),
    ('Editing', 4),
    ('Select text, then use Bold, Italic or Underline. Heading and Body apply to the current paragraph. Left, Center and Right control its alignment.', 0),
    ('Ctrl+F finds text. Ctrl+H replaces it. Replace All keeps untouched formatting and can be undone in one step. Ctrl+Z and Ctrl+Y undo and redo.', 0),
    ('Saving', 4),
    ('Ctrl+S saves a native .bwr document. Ctrl+Shift+S creates a new named copy. Existing unrelated names are never replaced.', 0),
    ('Ctrl+Shift+E exports a separate .rtf file for another word processor. Export does not save the native document or clear its unsaved marker.', 0),
    ('Ctrl+Shift+P exports a separate PDF. Choose Letter or A4, then a new .pdf name. PDF pages are for a host viewer or printer; export does not save the native document.', 0),
    ('Recovery', 4),
    ('Changed work is kept in a recovery draft while the desktop is idle. New, Open and Close ask before discarding changes. Cancel keeps the document.', 0),
    ('A recovered draft only saves back to an unchanged original file. If the source changed while the machine was stopped, choose a new name with Save As.', 0),
    ('Files and native apps', 4),
    ('Files supports Ctrl+C, Ctrl+X and Ctrl+V for copies and moves. Ctrl+F filters the current folder; Ctrl+1 through Ctrl+4 choose Name, Type, Size or Modified sorting. Open a second window with Ctrl+N.', 0),
    ('Open .bex programs from Files or the launcher. In Terminal, start /Programs/docstats.bex /Documents/stats-sample.txt scans that document. S saves its report; Q exits. Existing installed examples are never overwritten automatically.', 0),
    ('Limits', 4),
    ('Writer supports 32,768 ASCII text bytes, eight grouped undo operations and one window. Plain-text imports accept LF or CRLF line endings.', 0),
    ('RTF and PDF are export-only. PDF uses standard Times fonts and a 512 KiB output limit. Unicode, DOCX, embedded images, paginated editing and guest printer drivers are not implemented.', 0),
]


def document():
    text = ''.join(line + '\n' for line, _ in PARAGRAPHS).encode('ascii')
    assert len(text) <= 32768
    styles = bytearray(len(text) + 1); paragraphs = bytearray(len(text) + 1)
    offset = 0
    for line, flags in PARAGRAPHS:
        count = len(line.encode('ascii'))
        paragraphs[offset] = flags
        if flags & 4:
            styles[offset:offset + count] = bytes([1]) * count
        offset += count + 1
    return struct.pack('<4sHHII', b'BWR1', 1, 0, len(text), 0) + text + styles + paragraphs


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=pathlib.Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = document(); args.output.write_bytes(result)
    print(f'{args.output}: {len(result)} native bytes')
