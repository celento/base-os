"""Exercise the production native interrupt publication boundaries on the host.

The dispatcher and task state transitions are extracted without rewriting their
logic. Only the two privileged interrupt-flag instructions around fs_sync are
replaced; the publication checks never execute that unrelated syscall.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class NativePublicationProcessTests(unittest.TestCase):
    def test_production_interrupt_boundaries(self):
        source = (ROOT / 'src/process.c').read_text()
        first = source.index('#define TASK_KEYS ')
        last = source.index('static NativeTask *tasks=', first)
        types = source[first:last]
        for name, declaration in (('process_task_stop', 'void'),
                                  ('process_interrupt', 'int')):
            source = source.replace(declaration + ' ' + name + '(',
                                    'static ' + declaration + ' ' + name + '(')
        code = ''.join(function(source, name) for name in
                       ('current_owner', 'release_owner', 'task_release', 'task_at',
                        'task_suspend', 'finish', 'user_range', 'user_path',
                        'abi_query', 'native_file_call', 'process_task_stop', 'process_interrupt'))
        # The shared extractor recognizes static definitions; retain these two
        # functions' original external linkage in the generated host include.
        code = code.replace('static void process_task_stop(', 'void process_task_stop(')
        code = code.replace('static int process_interrupt(', 'int process_interrupt(')
        privileged = (
            ('__asm__ volatile("pushfl; popl %0; sti":"=r"(flags)::"memory");',
             'flags=0; /* Host shim: never execute privileged STI. */'),
            ('__asm__ volatile("pushl %0; popfl"::"r"(flags):"memory","cc");',
             '(void)flags; /* Host shim: no hardware interrupt flags to restore. */'),
        )
        for original, replacement in privileged:
            self.assertEqual(code.count(original), 1,
                             'Re-review the narrowly scoped fs_sync host shim')
            code = code.replace(original, replacement)
        self.assertNotIn('__asm__', code)
        with tempfile.TemporaryDirectory(prefix='baseos-native-publication-process-') as temporary:
            directory = Path(temporary)
            (directory / 'native_publication_process_types.inc').write_text(types)
            (directory / 'native_publication_process_ops.inc').write_text(code)
            executable = directory / 'native_publication_process_host'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/native_publication_process_host.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, env=dict(
                os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
