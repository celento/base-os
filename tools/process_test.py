"""Boot disposable native isolation tests; never edits the user image."""
import pathlib,subprocess,tempfile,sys,shutil
from layout import constants
from smoke_test import run
build=pathlib.Path(sys.argv[1]).resolve();root=pathlib.Path(__file__).resolve().parents[1]
d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-process-'));print(d,flush=True)
def tool(n):return 'x86_64-elf-'+n if shutil.which('x86_64-elf-'+n) else n
subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(root/'src'),'-c',str(root/'tests/process_guest.c'),'-o',str(d/'guest.o')],check=True)
subprocess.run(['nasm','-f','elf','-Dkmain=process_guest','-p',str(build/'layout.inc'),str(root/'src/kernel_entry.asm'),'-o',str(d/'entry.o')],check=True)
objects=[str(p) for p in build.glob('*.o') if p.name!='kernel_entry.o']
subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'entry.o'),str(d/'guest.o'),*objects],check=True)
subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
c=constants();kernel=(d/'kernel.bin').read_bytes();assert len(kernel)<=c['KERNEL_SECTORS']*512
image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();image[512:512+len(kernel)]=kernel
(d/'disk.img').write_bytes(image)
run(d/'disk.img',d,'isolation','PROCESS-ISOLATION-PASS',seconds=20)
