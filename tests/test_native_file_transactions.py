"""Bounded ordinary API fixtures. No guest/fault/corruption/security/fuzz run."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]

class FileTransactionTests(unittest.TestCase):
    def test_exact_dispatcher(self):
        with tempfile.TemporaryDirectory(prefix='baseos-transaction-dispatch-') as temporary:
            directory=Path(temporary)
            (directory/'transaction_dispatch.inc').write_text(function((ROOT/'src/process.c').read_text(),'native_file_transaction_call'))
            executable=directory/'dispatcher'
            subprocess.run([shutil.which('cc') or 'gcc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'src'),'-I',str(directory),str(ROOT/'tests/native_file_transaction_dispatch_host.c'),
                '-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)

    def test_sdk_confirmation(self):
        with tempfile.TemporaryDirectory(prefix='baseos-transaction-sdk-') as temporary:
            directory=Path(temporary)
            sdk=(ROOT/'sdk/baseos.h').read_text()
            (directory/'transaction_sdk.inc').write_text(function(sdk,'bos_file_transaction_query')+function(sdk,'bos_file_sync_revision'))
            executable=directory/'sdk'
            subprocess.run([shutil.which('cc') or 'gcc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
                '-Wno-pointer-to-int-cast','-I',str(ROOT/'src'),'-I',str(directory),str(ROOT/'tests/file_transaction_sdk_host.c'),
                '-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)

    def test_ordinary_profiles(self):
        with tempfile.TemporaryDirectory(prefix='baseos-file-transactions-') as temporary:
            for profile, ram in (('default',64),('large',128)):
                executable=Path(temporary)/profile
                subprocess.run([shutil.which('cc') or 'gcc','-std=gnu11','-O1','-g',
                    '-Wall','-Wextra','-Werror','-Wno-unused-function','-I',str(ROOT/'src'),
                    *(['-DTRANSACTION_LARGE_PROFILE'] if profile=='large' else []),
                    str(ROOT/'tests/native_file_transactions_host.c'),str(ROOT/'src/physmem.c'),
                    str(ROOT/'tests/native_file_stage_bootinfo.c'),'-o',str(executable)],check=True)
                subprocess.run([str(executable),str(ram)],check=True)

if __name__=='__main__':
    unittest.main()
