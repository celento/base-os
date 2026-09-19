# BaseOS

UNAME_S := $(shell uname -s)

AS = nasm

ifeq ($(UNAME_S),Darwin)
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy
FILESIZE = stat -f%z
QEMU_DISPLAY = cocoa
else
CC = gcc
LD = ld
OBJCOPY = objcopy
FILESIZE = stat -c%s
QEMU_DISPLAY = gtk
endif

SRC = src
OUT = build
PYTHON ?= python3

CFLAGS = -std=gnu11 -ffreestanding -Os -g -Wall -Wextra -m32 \
	-nostdlib -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
	-mno-sse -mno-mmx -msoft-float -I. -I$(SRC) -I$(OUT)
ASFLAGS = -f elf
LDFLAGS = -T $(OUT)/linker.ld -nostdlib -m elf_i386 -z noexecstack

CSRC = basic.c process.c history.c platform.c bootinfo.c kernel.c gfx.c fs.c persist.c wordle.c term.c todo.c rtc.c clock.c calendar.c mines.c game2048.c breakout.c sysmon.c
OBJS = $(OUT)/kernel_entry.o $(OUT)/interrupts.o $(OUT)/process_entry.o $(addprefix $(OUT)/,$(CSRC:.c=.o))
HDRS = $(wildcard $(SRC)/*.h) $(wildcard assets/*.h)
IMG = $(OUT)/baseos.img

all: $(IMG)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/layout.inc: $(SRC)/layout.h tools/layout.py | $(OUT)
	$(PYTHON) tools/layout.py > $@

$(OUT)/linker.ld: $(SRC)/linker.ld $(SRC)/layout.h | $(OUT)
	$(CC) -E -P -x c -I$(SRC) $< -o $@

$(OUT)/boot.bin: $(SRC)/boot.asm $(OUT)/layout.inc Makefile | $(OUT)
	$(AS) -f bin -p $(OUT)/layout.inc $< -o $@

$(OUT)/process_entry.o: $(SRC)/process_entry.asm $(OUT)/layout.inc Makefile | $(OUT)
	$(AS) $(ASFLAGS) -p $(OUT)/layout.inc $< -o $@

$(OUT)/%.o: $(SRC)/%.asm $(OUT)/layout.inc Makefile | $(OUT)
	$(AS) $(ASFLAGS) -p $(OUT)/layout.inc $< -o $@

$(OUT)/%.o: $(SRC)/%.c $(HDRS) Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/hello.bex: examples/hello.asm | $(OUT)
	$(AS) -f bin $< -o $@

$(OUT)/native_example.h: $(OUT)/hello.bex tools/bin2c.py
	$(PYTHON) tools/bin2c.py $< > $@

$(OUT)/kernel.o: $(OUT)/native_example.h

# Rendering is the hot path; retain size optimization for the rest of the kernel.
$(OUT)/gfx.o: CFLAGS := $(filter-out -Os,$(CFLAGS)) -O2

$(OUT)/kernel.elf: $(OBJS) $(OUT)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(IMG): $(OUT)/boot.bin $(OUT)/kernel.bin tools/update_image.py tools/layout.py $(SRC)/layout.h
	$(PYTHON) tools/update_image.py $@ $(OUT)/boot.bin $(OUT)/kernel.bin

run: $(IMG)
	qemu-system-i386 -m 32M -vga std \
		-drive file=$(IMG),format=raw,index=0,if=floppy \
		-serial file:$(OUT)/serial.out -no-reboot -display $(QEMU_DISPLAY)

headless: $(IMG)
	qemu-system-i386 -m 32M -vga std \
		-drive file=$(IMG),format=raw,index=0,if=floppy \
		-serial stdio -display none -no-reboot

clean:
	rm -f $(OBJS) $(OUT)/boot.bin $(OUT)/kernel.bin $(OUT)/kernel.elf $(OUT)/linker.ld $(OUT)/layout.inc

.PHONY: all run headless clean

# Image and its backups intentionally survive clean.
test:
	$(PYTHON) -m unittest discover -s tests -p "test_*.py"

.PHONY: test
