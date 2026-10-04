"""Select unchanged production process logic, leaving hardware code outside tests.

The two legacy sync interrupt-flag instructions remain the existing narrow host
shim. Creation, identity, lifecycle, dispatcher, scheduler and copying are real
production functions. No native bytecode or privileged instruction is executed.
"""
import re
from test_editor_binding import function

FUNCTIONS = (
    'task_private', 'task_private_release', 'task_private_copy', 'task_private_create',
    'allocate_owner', 'current_owner', 'release_owner', 'task_release',
    'task_mark_exit', 'task_finalize', 'task_image_write', 'task_image_read',
    'task_reset_all', 'valid_image', 'image_magic', 'image_plan', 'process_probe_launch', 'task_lookup',
    'process_create_mode', 'process_create', 'process_launch_mode', 'process_bind', 'process_unbind', 'process_start',
    'process_status', 'process_binding_live', 'process_get_result', 'process_counts', 'task_wake',
    'process_key', 'process_request_stop', 'process_reap',
    'output_print', 'output_plot', 'output_present', 'output_resize', 'output_rect',
    'task_suspend', 'finish', 'user_extent', 'user_span', 'user_path', 'abi_query', 'memory_region', 'memory_info',
    'native_file_call', 'native_ui_call', 'process_interrupt',
)


def extract(source, directory, stem, scheduler=False):
    first = source.index('#define TASK_KEYS ')
    types = source[first:source.index('static NativeTask *tasks=', first)]
    exported = re.findall(r'^(int|void|unsigned|ProcessHandle) (process_\w+)\(', source, re.M)
    for result, name in exported:
        source = source.replace(result + ' ' + name + '(', 'static ' + result + ' ' + name + '(')
    names = FUNCTIONS + (('process_run', 'process_step', 'process_schedule_one') if scheduler else ())
    code = ''.join(function(source, name) for name in names)
    for result, name in exported:
        code = code.replace('static ' + result + ' ' + name + '(', result + ' ' + name + '(')
    for old, new in (
        ('__asm__ volatile("pushfl; popl %0; sti":"=r"(flags)::"memory");', 'flags=0;'),
        ('__asm__ volatile("pushl %0; popfl"::"r"(flags):"memory","cc");', '(void)flags;'),
    ):
        assert code.count(old) == 1, 'Re-review the legacy sync host shim'
        code = code.replace(old, new)
    assert '__asm__' not in code
    (directory / (stem + '_types.inc')).write_text(types)
    (directory / (stem + '_ops.inc')).write_text(code)
