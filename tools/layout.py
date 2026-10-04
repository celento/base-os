"""The integer-only shared layout; reject ambiguous configuration."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

def constants():
    text = (ROOT / 'src/layout.h').read_text()
    result = {k: int(v, 0) for k, v in re.findall(
        r'^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$', text, re.M)}
    validate(result)
    return result

def validate(c):
    """Keep assembler generation and every disk tool inside the same bounds."""
    stage, limit, load = (c[n] for n in
                          ('KERNEL_STAGE_ADDR', 'KERNEL_STAGE_LIMIT', 'KERNEL_LOAD_ADDR'))
    sectors, primary, sector = (c[n] for n in
                                ('KERNEL_SECTORS', 'KERNEL_PRIMARY_SECTORS', 'SECTOR_SIZE'))
    if (stage < 0x10000 or stage % 16 or load % 16 or
        stage + sectors * sector > limit or limit > 0xA0000 or
        limit > load or load < 0x100000):
        raise ValueError('invalid BIOS staging/relocated kernel layout')
    if (c['KERNEL_BOOTSTRAP_BYTES'] != 4096 or c['KERNEL_PACK_HEADER_BYTES'] != 32 or
        c['KERNEL_BOOT_STACK_BOTTOM'] < 0x8400 or
        not c['KERNEL_BOOT_STACK_BOTTOM'] < c['KERNEL_BOOT_STACK_TOP'] <= stage):
        raise ValueError('invalid packed bootstrap layout')
    if (not load < c['STACK_BOTTOM'] or
        c['STACK_TOP'] - c['STACK_BOTTOM'] != 0x10000 or
        c['STACK_TOP'] > c['FB_BASE']):
        raise ValueError('invalid kernel stack/framebuffer layout')
    if (sector != 512 or not 0 < primary <= sectors or
        primary + 1 > c['FS_DISK_LBA'] or
        c['FS_SECOND_LBA'] < c['FS_DISK_LBA'] + c['FS_DISK_SECTORS'] or
        c['KERNEL_EXT_LBA'] < c['FS_SECOND_LBA'] + c['FS_DISK_SECTORS'] or
        c['KERNEL_EXT_LBA'] + sectors - primary > c['DISK_SECTORS']):
        raise ValueError('invalid split kernel/filesystem disk layout')


if __name__ == '__main__':
    for name, value in constants().items():
        print(f'%define {name} {value}')
