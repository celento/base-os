"""Check maximum-size draft restoration and all-or-nothing session capacity preflight."""
import pathlib,subprocess,tempfile,sys,shutil,json,time
from layout import constants
from update_image import install_kernel
from data_volume_test import run
from init_data import initialize
build=pathlib.Path(sys.argv[1]).resolve();root=pathlib.Path(__file__).resolve().parents[1]
d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-session-draft-'));print(d,flush=True)
def tool(n):return 'x86_64-elf-'+n if shutil.which('x86_64-elf-'+n) else n
subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(root),'-I',str(root/'src'),'-I',str(build),'-c',str(root/'tests/session_draft_guest.c'),'-o',str(d/'kernel.o')],check=True)
objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'kernel.o'),*objects],check=True)
subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
c=constants();kernel=(d/'kernel.bin').read_bytes();assert len(kernel)<=c['KERNEL_SECTORS']*512
image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();install_kernel(image,kernel,c)
(d/'disk.img').write_bytes(image)
initialize(d/'data.img')
run(d/'disk.img',d/'data.img',d,'draft','SESSION-DRAFT-PASS',seconds=60)
run(d/'disk.img',d/'data.img',d,'draft-reboot','SESSION-DRAFT-REBOOT-PASS',seconds=40)
print('Maximum 65535-byte and 65534-byte unsaved drafts, full-volume preflight, every-byte reboot recovery and caret positions passed.',flush=True)
