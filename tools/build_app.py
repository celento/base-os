"""Build a freestanding C application as a protected BaseOS BEX1 executable."""
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
def build(source, output):
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
    assert magic==0x31584542 and 16<=entry<len(data)==length and not reserved and length<=16383
    print(f'{output}: {length} bytes, BEX1 entry 0x{entry:x}')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=pathlib.Path);parser.add_argument('output',type=pathlib.Path)
    args=parser.parse_args();build(args.source,args.output)
