"""Normal protected multitasking/cancellation tests on a disposable QEMU disk."""
import pathlib, subprocess, tempfile, sys, shutil
from layout import constants
from update_image import install_kernel
from smoke_test import run

build=pathlib.Path(sys.argv[1]).resolve()
root=pathlib.Path(__file__).resolve().parents[1]
d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-tasks-'));print(d,flush=True)
def tool(name):return 'x86_64-elf-'+name if shutil.which('x86_64-elf-'+name) else name
subprocess.run([sys.executable,str(root/'tools/build_app.py'),str(root/'tests/task_app.c'),str(d/'task-app.bex')],check=True)
for label,value in [('a',111),('b',222)]:
    subprocess.run(['nasm','-f','bin',f'-DVALUE={value}',str(root/'tests/task_fpu.asm'),'-o',str(d/f'fpu-{label}.bex')],check=True)
with (d/'task_examples.h').open('w') as out:
    for name,file in [('task_app','task-app.bex'),('task_fpu_a','fpu-a.bex'),('task_fpu_b','fpu-b.bex')]:
        subprocess.run([sys.executable,str(root/'tools/bin2c.py'),str(d/file),name],stdout=out,check=True)
subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(root),'-I',str(root/'src'),'-I',str(build),'-I',str(d),'-c',str(root/'tests/task_guest.c'),'-o',str(d/'kernel.o')],check=True)
objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'kernel.o'),*objects],check=True)
subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
c=constants();kernel=(d/'kernel.bin').read_bytes();assert len(kernel)<=c['KERNEL_SECTORS']*512
image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();install_kernel(image,kernel,c)
(d/'disk.img').write_bytes(image)
run(d/'disk.img',d,'tasks','NATIVE-TASK-RUNTIME-PASS',memory='64M',seconds=60)
print('Independent long-running tasks, preemption, x87 isolation, input, sleep, stop/restart, terminal ownership and legacy exec passed.',flush=True)
