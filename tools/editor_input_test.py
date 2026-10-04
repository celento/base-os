"""Find/replace through real PS/2 input in the unmodified desktop kernel."""
import pathlib
import sys
import time
from qemu_session import DesktopSession
with DesktopSession(pathlib.Path(sys.argv[1]),'editor-input') as session:
    print(session.directory,flush=True);session.boot();session.launch('editor')
    session.text('one fish TWO fish\nA small document edited with the keyboard.')
    session.key('ctrl-h');session.text('fish');session.key('tab');session.text('bird');session.key('ctrl-shift-ret')
    time.sleep(.2)
    memory=session.memory(session.layout['EDITOR_BASE'],200000)
    assert b'one bird TWO bird\nA small document edited with the keyboard.\x00' in memory
    session.screenshot('editor-replace.png')
    session.key('ctrl-z');session.key('esc')
    memory=session.memory(session.layout['EDITOR_BASE'],200000)
    assert b'one fish TWO fish\nA small document edited with the keyboard.\x00' in memory
    session.key('ctrl-home');session.key('shift-right');session.key('shift-right');session.key('delete')
    memory=session.memory(session.layout['EDITOR_BASE'],200000)
    assert b'e fish TWO fish\nA small document edited with the keyboard.\x00' in memory
    session.screenshot('editor-selection.png')
    print('Real PS/2 replace-all, undo, search dismissal, Shift selection and Delete passed.',flush=True)
