"""Exercise desktop integration and session reboot using disposable QEMU images."""
import pathlib,subprocess,tempfile,sys,shutil,json,time
from layout import constants
from smoke_test import run
build=pathlib.Path(sys.argv[1]).resolve();root=pathlib.Path(__file__).resolve().parents[1]
d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-render-'));print(d,flush=True)
def tool(n):return 'x86_64-elf-'+n if shutil.which('x86_64-elf-'+n) else n
subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(root),'-I',str(root/'src'),'-I',str(build),'-c',str(root/'tests/render_guest.c'),*(['-DRENDER_OPTIMIZED'] if '--optimized' in sys.argv else []),'-o',str(d/'kernel.o')],check=True)
objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'kernel.o'),*objects],check=True)
subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
c=constants();kernel=(d/'kernel.bin').read_bytes();assert len(kernel)<=c['KERNEL_SECTORS']*512
image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();image[512:512+len(kernel)]=kernel
(d/'disk.img').write_bytes(image)
run(d/'disk.img',d,'render','RENDER-PASS',seconds=60)
print((d/'render.log').read_text(),flush=True)
