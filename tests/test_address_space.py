"""Deterministic native mapping inspection, not a guest memory probe."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class AddressSpaceTests(unittest.TestCase):
    def test_checked_spans(self):
        with tempfile.TemporaryDirectory(prefix='baseos-spans-') as temp:
            exe=Path(temp)/'spans'
            subprocess.run([shutil.which('clang') or 'cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-I',str(ROOT/'src'),str(ROOT/'tests/address_space_host.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
    def test_copy_audit_and_restore_order(self):
        source=(ROOT/'src/process.c').read_text()
        self.assertNotIn('user_range(',source)
        self.assertNotIn('b>USER_CAPACITY-a',source)
        self.assertIn('user_span(b,c,USER_READ)||e<sizeof(info)||!user_span(d,e,USER_WRITE)',source)
        step=source[source.index('int process_step('):source.index('ProcessHandle process_schedule_one(')]
        self.assertLess(step.index('process_resume(task->frame);'),step.index('process_kernel_context();'))
        self.assertLess(step.index('process_kernel_context();'),step.index('fpu_leave(task);'))
        self.assertLess(step.index('fpu_leave(task);'),step.index('task_finalize(task);',step.index('fpu_leave(task);')))
        run=source[source.index('int process_run('):source.index('static NativeTask *task_lookup')]
        self.assertLess(run.index('process_kernel_context();'),run.index('release_owner(synchronous_owner);'))
if __name__=='__main__': unittest.main()
