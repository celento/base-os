"""Real gated process/table/allocator logic with ordinary linked apps, no QEMU."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract
from test_editor_binding import function
ROOT=Path(__file__).resolve().parents[1]
class PrivateAddressSpaceTests(unittest.TestCase):
    def test_owned_spaces_and_dispatch(self):
        with tempfile.TemporaryDirectory(prefix='baseos-private-spaces-') as temp:
            directory=Path(temp)
            source=(ROOT/'src/process.c').read_text()
            extract(source,directory,'private_space',scheduler=True)
            hardware_free=''.join(function(source,name) for name in ('descriptor','process_user_root','document_parent','file_call'))
            (directory/'private_space_helpers.inc').write_text(hardware_free)
            images=[]
            for name,workspace in [('one',1048576),('capacity',1966080),('three',3145728)]:
                image=directory/(name+'.bex')
                subprocess.run(['python3',str(ROOT/'tools/build_app.py'),str(ROOT/'tests/private_space_app.c'),str(image),
                    '--format','bex2','--workspace-bytes',str(workspace)],check=True)
                images.append(str(image))
            executable=directory/'spaces'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                '-Wno-unused-function','-fsanitize=address,undefined','-DBASEOS_BEX2_ENABLED=1',
                '-I',str(ROOT/'src'),'-I',str(directory),str(ROOT/'tests/private_address_space_host.c'),
                str(ROOT/'src/physmem.c'),str(ROOT/'src/bootinfo.c'),str(ROOT/'src/executable.c'),'-o',str(executable)],check=True)
            for ram in (64,128,256):
                subprocess.run([str(executable),str(ram),*images],check=True,env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
if __name__=='__main__': unittest.main()
