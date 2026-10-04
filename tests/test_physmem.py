"""Real bounded allocator core; ordinary host fixtures and compile-only i386 layout.

No QEMU, injected guest faults, random/fuzz input or physical memory probes.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PhysmemTests(unittest.TestCase):
    def test_production_core(self):
        source = (ROOT / 'src/process.c').read_text()
        start = source.index('static void protect_memory(void)')
        end = source.index('    unsigned cr4,cr0;', start)
        construction = source[start:end] + '}\n'
        self.assertNotIn('__asm__', construction)
        with tempfile.TemporaryDirectory(prefix='baseos-owned-pages-') as temporary:
            directory = Path(temporary)
            (directory / 'physmem_paging_construction.inc').write_text(construction)
            executable = directory / 'physmem'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                            '-I', str(directory), str(ROOT / 'tests/physmem_host.c'),
                            str(ROOT / 'src/physmem.c'), str(ROOT / 'src/bootinfo.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_i386_exact_task_layout(self):
        # Use the actual current type declaration, not a separately maintained
        # facsimile or host-pointer layout. Compile only; never execute process.c.
        source = (ROOT / 'src/process.c').read_text()
        first = source.index('#define TASK_KEYS ')
        types = source[first:source.index('static NativeTask *tasks=', first)]
        unit = '#include "program.h"\n#include "platform.h"\n#include "../sdk/baseos_abi.h"\n'
        unit += types
        unit += '''
_Static_assert(sizeof(void *) == 4, "must compile actual i386 layout");
_Static_assert(TASK_BASE + sizeof(NativeTask) * PROCESS_TASKS <= TASK_PAGE_METADATA_BASE,
               "actual task records overlap owned-page metadata");
_Static_assert(TASK_PAGE_METADATA_BASE + TASK_PAGE_METADATA_CAPACITY == TASK_INTERRUPT_STACK_BASE,
               "metadata reservation must end at unchanged syscall-stack boundary");
unsigned char measured_task_bytes[sizeof(NativeTask)];
unsigned char measured_table_bytes[sizeof(NativeTask) * PROCESS_TASKS];
'''
        with tempfile.TemporaryDirectory(prefix='baseos-owned-page-layout-') as temporary:
            directory = Path(temporary)
            path = directory / 'layout.c'
            path.write_text(unit)
            obj = directory / 'layout.o'
            subprocess.run([shutil.which('gcc') or 'cc', '-std=gnu11', '-m32',
                            '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'src'), '-c', str(path), '-o', str(obj)], check=True)
            symbols = subprocess.check_output(['nm', '-S', str(obj)], text=True)
            measured = {line.split()[-1]: int(line.split()[1], 16)
                        for line in symbols.splitlines() if 'measured_' in line}
            self.assertEqual(measured['measured_table_bytes'], 8 * measured['measured_task_bytes'])
            print('i386 NativeTask bytes:', measured['measured_task_bytes'],
                  'table bytes:', measured['measured_table_bytes'],
                  'table end:', hex(0x3000000 + measured['measured_table_bytes']))

    def test_late_initialization_order(self):
        source = (ROOT / 'src/kernel.c').read_text()
        main = source[source.index('void kmain(void)'):]
        self.assertLess(main.index('platform_validate_memory();'), main.index('video_init();'))
        self.assertLess(main.index('video_init();'), main.index('physmem_init()'))
        self.assertLess(main.index('physmem_init()'), main.index('audio_init();'))
        self.assertNotIn('physmem_init', (ROOT / 'src/platform.c').read_text())
        self.assertNotIn('physmem_init', (ROOT / 'src/process.c').read_text())


if __name__ == '__main__':
    unittest.main()
