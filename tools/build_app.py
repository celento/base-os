"""Build a freestanding C application: BEX1 by default, explicit BEX2/GUI modes."""
import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[1]
def tool(name):
    override=os.environ.get({'gcc':'CC','ld':'LD','objcopy':'OBJCOPY'}[name])
    return override or ('x86_64-elf-'+name if shutil.which('x86_64-elf-'+name) else name)
def _build_bex1(source, output):
    source=source.resolve();output=output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='baseos-app-') as temp:
        temp=pathlib.Path(temp);objects=[]
        for i,path in enumerate((ROOT/'sdk/start.c',source)):
            obj=temp/f'{i}.o';objects.append(str(obj))
            subprocess.run([tool('gcc'),'-std=c11','-Os','-Wall','-Wextra','-Werror','-m32',
                '-ffreestanding','-fno-pie','-fno-pic','-fno-stack-protector','-fno-builtin',
                '-mno-sse','-mno-mmx','-msoft-float','-I',str(ROOT/'sdk'),
                '-c',str(path),'-o',str(obj)],check=True)
        elf=temp/'app.elf'
        subprocess.run([tool('ld'),'-m','elf_i386','-T',str(ROOT/'sdk/app.ld'),'-nostdlib',
                        '-z','noexecstack','-o',str(elf),*objects],check=True)
        subprocess.run([tool('objcopy'),'-O','binary',str(elf),str(output)],check=True)
    data=output.read_bytes();magic,entry,length,reserved=struct.unpack_from('<4I',data)
    assert magic==0x31584542 and 16<=entry<len(data)==length and not reserved and length<=49152
    print(f'{output}: {length} bytes, BEX1 entry 0x{entry:x}' +
          (' (IDE data disk required for files over 16383 bytes)' if length>16383 else ''))

BEX2_PAGE = 4096
BEX2_EXTENT = 4 * 1024 * 1024
BEX2_FILE_MAX = 256 * 1024
BEX2_FLAG_NATIVE_WINDOW_V1 = 0x1
BEX2_NATIVE_WINDOW_ABI_MINOR = 2


def _bex2_header(data):
    """Verify tool output before publishing it. Kernel validation is independent."""
    if len(data) < 64:
        raise ValueError('BEX2 output lacks its 64-byte header')
    h = struct.unpack_from('<16I', data)
    magic, header, version, flags, size, entry, text, start, initialized, memory, workspace, stack, major, minor, r0, r1 = h
    if ((magic, header, version, major, r0, r1) != (0x32584542, 64, 1, 1, 0, 0)
            or flags & ~BEX2_FLAG_NATIVE_WINDOW_V1):
        raise ValueError('BEX2 output has unsupported header fields')
    if flags & BEX2_FLAG_NATIVE_WINDOW_V1 and minor < BEX2_NATIVE_WINDOW_ABI_MINOR:
        raise ValueError('BEX2 native-v1 windows require ABI minor 2 or newer')
    align = lambda n: (n + BEX2_PAGE - 1) & -BEX2_PAGE
    if not (text and 4096 <= entry < 4096 + text and start == align(4096 + text)
            and initialized <= memory and size == start + initialized
            and len(data) <= size <= BEX2_FILE_MAX):
        raise ValueError('BEX2 output has inconsistent file layout')
    if not (workspace % BEX2_PAGE == 0 and stack % BEX2_PAGE == 0
            and 16384 <= stack <= 262144
            and align(start + memory) + workspace <= BEX2_EXTENT - stack - BEX2_PAGE):
        raise ValueError('BEX2 output exceeds its committed memory layout')
    return h


def _validate_bex2_elf(path, header):
    """Reject loadable or deferred-runtime sections outside the fixed format."""
    data = path.read_bytes()
    if len(data) < 52:
        raise ValueError('BEX2 linker did not produce ELF32')
    ident, kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, names_index = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if ident[:7] != b'\x7fELF\x01\x01\x01' or (kind, machine, version, entry) != (2, 3, 1, header[5]):
        raise ValueError('BEX2 requires a static little-endian i386 ELF with matching entry')
    if ehsize != 52 or shsize != 40 or not shnum or names_index >= shnum or shoff + shsize * shnum > len(data):
        raise ValueError('BEX2 ELF section table is inconsistent')
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize) for i in range(shnum)]
    names_section = sections[names_index]
    names = data[names_section[4]:names_section[4] + names_section[5]]
    for section in sections:
        name_at, section_type, section_flags, address, offset, size = section[:6]
        name = names[name_at:].split(b'\0', 1)[0]
        if size and (section_type in (4, 6, 9, 11) or section_flags & 0x400):
            raise ValueError('BEX2 does not support relocation, dynamic or TLS sections')
        if not (section_flags & 2 and size):
            continue
        if section_type != 8 and offset + size > len(data):
            raise ValueError('BEX2 ELF payload is truncated')
        ranges = {
            b'.header': (0, 64, False, 1),
            b'.text': (4096, 4096 + header[6], False, 1),
            b'.rodata': (4096, 4096 + header[6], False, 1),
            b'.data': (header[7], header[7] + header[8], True, 1),
            b'.bss': (header[7], header[7] + header[9], True, 8),
        }
        if name not in ranges:
            raise ValueError(f'BEX2 has an unsupported allocated section: {name!r}')
        low, high, writable, expected_type = ranges[name]
        if address < low or address + size > high or bool(section_flags & 1) != writable or section_type != expected_type:
            raise ValueError(f'BEX2 section disagrees with header layout: {name!r}')
    if phnum and (phsize != 32 or phoff + phnum * phsize > len(data)):
        raise ValueError('BEX2 ELF program header table is inconsistent')
    for i in range(phnum):
        if struct.unpack_from('<I', data, phoff + i * phsize)[0] in (2, 3):
            raise ValueError('BEX2 does not support a dynamic linker or interpreter')


def _build_bex2(source, output, workspace_bytes, stack_bytes, required_abi_minor, elf_output, flags):
    for name, value in (('workspace', workspace_bytes), ('stack', stack_bytes)):
        if not isinstance(value, int) or value < 0 or value > BEX2_EXTENT or value % BEX2_PAGE:
            raise ValueError(f'BEX2 {name} must be a nonnegative page multiple within 4 MiB')
    if not 16384 <= stack_bytes <= 262144:
        raise ValueError('BEX2 stack must be between 16 and 256 KiB')
    if not isinstance(required_abi_minor, int) or not 0 <= required_abi_minor <= 0xffffffff:
        raise ValueError('BEX2 required ABI minor must fit an unsigned 32-bit word')
    source = source.resolve()
    output = output.resolve()
    if elf_output is not None:
        elf_output = elf_output.resolve()
        if elf_output == output:
            raise ValueError('BEX2 executable and optional ELF output must be distinct')
    with tempfile.TemporaryDirectory(prefix='baseos-app2-') as temporary:
        temp = pathlib.Path(temporary)
        objects = []
        defines = ['-DBOS_APP_NATIVE_WINDOW_V1=1'] if flags & BEX2_FLAG_NATIVE_WINDOW_V1 else []
        for i, path in enumerate((ROOT / 'sdk/start2.c', source)):
            obj = temp / f'{i}.o'
            objects.append(str(obj))
            subprocess.run([tool('gcc'), '-std=c11', '-Os', '-Wall', '-Wextra', '-Werror', '-m32',
                '-ffreestanding', '-fno-pie', '-fno-pic', '-fno-stack-protector', '-fno-builtin',
                '-mno-sse', '-mno-mmx', '-msoft-float', *defines, '-I', str(ROOT / 'sdk'),
                '-c', str(path), '-o', str(obj)], check=True)
        elf = temp / 'app.elf'
        subprocess.run([tool('ld'), '-m', 'elf_i386',
            '--defsym=__bex2_workspace_bytes=' + str(workspace_bytes),
            '--defsym=__bex2_stack_bytes=' + str(stack_bytes),
            '--defsym=__bex2_required_abi_minor=' + str(required_abi_minor),
            '--defsym=__bex2_flags=' + str(flags),
            '-T', str(ROOT / 'sdk/app2.ld'), '-nostdlib', '-z', 'noexecstack',
            '-o', str(elf), *objects], check=True)
        binary = temp / 'app.bex'
        subprocess.run([tool('objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
        data = binary.read_bytes()
        header = _bex2_header(data)
        if header[3] != flags or header[13] != required_abi_minor:
            raise ValueError('BEX2 linker header disagrees with the requested launch mode or ABI minor')
        _validate_bex2_elf(elf, header)
        if header[8] and len(data) != header[4]:
            raise ValueError('BEX2 initialized data payload is truncated')
        # objcopy omits trailing alignment when initialized data is empty.
        # The wire format intentionally preserves file==virtual data offsets.
        data += bytes(header[4] - len(data))
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        if elf_output is not None:
            elf_output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(elf, elf_output)
    print(f'{output}: {len(data)} bytes, BEX2 entry 0x{header[5]:x}, '
          f'workspace {workspace_bytes} bytes, stack {stack_bytes} bytes'
          + ('; native-v1 window' if flags & BEX2_FLAG_NATIVE_WINDOW_V1 else '')
          + (' (IDE data disk required for files over 16383 bytes)' if len(data) > 16383 else ''))


def build(source, output, *, format='bex1', workspace_bytes=None, stack_bytes=None,
          required_abi_minor=None, elf_output=None, window=None):
    if format == 'bex1':
        if any(value is not None for value in (workspace_bytes, stack_bytes, required_abi_minor, elf_output, window)):
            raise ValueError('BEX2 options require --format bex2; the default BEX1 builder is unchanged')
        return _build_bex1(source, output)
    if format != 'bex2':
        raise ValueError('format must be bex1 or bex2')
    if window not in (None, 'native-v1'):
        raise ValueError('window must be native-v1 when specified')
    if window is not None and required_abi_minor is not None:
        if not isinstance(required_abi_minor, int) or required_abi_minor < BEX2_NATIVE_WINDOW_ABI_MINOR:
            raise ValueError('BEX2 native-v1 windows require ABI minor 2 or newer')
    minor = (BEX2_NATIVE_WINDOW_ABI_MINOR if window is not None else 1) if required_abi_minor is None else required_abi_minor
    return _build_bex2(source, output,
        1048576 if workspace_bytes is None else workspace_bytes,
        65536 if stack_bytes is None else stack_bytes,
        minor, elf_output, BEX2_FLAG_NATIVE_WINDOW_V1 if window is not None else 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--format', choices=('bex1', 'bex2'), default='bex1')
    parser.add_argument('--window', choices=('native-v1',),
                        help='require an owned native window (requires --format bex2 and ABI minor >=2)')
    parser.add_argument('--workspace-bytes', type=lambda s: int(s, 0))
    parser.add_argument('--stack-bytes', type=lambda s: int(s, 0))
    parser.add_argument('--required-abi-minor', type=lambda s: int(s, 0))
    parser.add_argument('--elf-output', type=pathlib.Path, help='retain the BEX2 ELF for symbol inspection')
    args = parser.parse_args()
    try:
        build(args.source, args.output, format=args.format, workspace_bytes=args.workspace_bytes,
              stack_bytes=args.stack_bytes, required_abi_minor=args.required_abi_minor,
              elf_output=args.elf_output, window=args.window)
    except ValueError as error:
        parser.error(str(error))
