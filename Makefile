# BaseOS Makefile

AS = nasm
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

CFLAGS = -std=gnu99 -ffreestanding -O0 -g -Wall -Wextra -m32 -nostdlib -I.
ASFLAGS = -f elf
LDFLAGS = -T linker.ld -nostdlib -m elf_i386

all: baseos.img

boot.bin: boot.asm
	$(AS) -f bin $< -o $@

kernel_entry.o: kernel_entry.asm
	$(AS) $(ASFLAGS) $< -o $@

kernel.o: kernel.c font.h
	$(CC) $(CFLAGS) -c kernel.c -o $@

kernel.elf: kernel_entry.o kernel.o
	$(LD) $(LDFLAGS) -o $@ kernel_entry.o kernel.o

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

baseos.img: boot.bin kernel.bin
	cat boot.bin kernel.bin > baseos.img.tmp
	dd if=/dev/zero bs=512 count=2880 >> baseos.img.tmp 2>/dev/null || true
	dd if=baseos.img.tmp of=baseos.img bs=512 count=2880
	rm baseos.img.tmp

run: baseos.img
	qemu-system-i386 -drive file=baseos.img,format=raw,index=0,if=floppy -serial file:serial.out -no-reboot -d int,cpu_reset,guest_errors -D qemu_log.txt

clean:
	rm -f boot.bin kernel.o kernel_entry.o kernel.elf kernel.bin baseos.img baseos.img.tmp *.o serial.out qemu_log.txt

.PHONY: all run clean