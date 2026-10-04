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
QEMU_AUDIO ?= -device sb16

CFLAGS = -std=gnu11 -ffreestanding -Os -g -Wall -Wextra -m32 \
	-nostdlib -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
	-mno-sse -mno-mmx -msoft-float -I. -I$(SRC) -I$(OUT)
ASFLAGS = -f elf
LDFLAGS = -T $(OUT)/linker.ld -nostdlib -m elf_i386 -z noexecstack

CSRC = example_sheet.c sheet.c sheet_model.c sheet_codec.c file_clipboard.c file_view.c example_docs.c writer.c writer_codec.c writer_pdf.c download.c video.c video_draw.c image_decode.c image_viewer.c text_search.c ata.c player.c browser.c net.c net_wire.c net_rtl8139.c audio.c media.c media_mp3.c display.c decimal.c basic.c process.c executable.c app_storage.c app_canvas.c app_view.c canvas_view.c native_ui.c native_files.c native_sync.c kernel_owner.c document_save.c history.c platform.c bootinfo.c physmem.c input_ingress.c kernel.c gfx.c fs.c persist.c wordle.c term.c todo.c rtc.c clock.c calendar.c calendar_agenda.c mines.c game2048.c breakout.c sysmon.c
OBJS = $(OUT)/kernel_entry.o $(OUT)/interrupts.o $(OUT)/process_entry.o $(addprefix $(OUT)/,$(CSRC:.c=.o))
HDRS = $(wildcard $(SRC)/*.h) $(wildcard assets/*.h) $(wildcard sdk/*.h)
IMG = $(OUT)/baseos.img
DATA_IMG = $(OUT)/baseos-data.img
DATA_PROFILE ?= default
QEMU_MEMORY ?= 64M
QEMU_DATA = -drive file=$(DATA_IMG),format=raw,index=0,if=ide,cache=writeback

all: $(IMG) $(DATA_IMG)

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

$(OUT)/hello-c.bex: examples/c/hello.c sdk/baseos.h sdk/baseos_abi.h sdk/start.c sdk/app.ld tools/build_app.py | $(OUT)
	$(PYTHON) tools/build_app.py $< $@

$(OUT)/notebook.bex: examples/c/notebook.c sdk/baseos.h sdk/baseos_abi.h sdk/start.c sdk/app.ld tools/build_app.py | $(OUT)
	$(PYTHON) tools/build_app.py $< $@

$(OUT)/counter.bex: examples/c/counter.c sdk/baseos.h sdk/baseos_abi.h sdk/start.c sdk/app.ld tools/build_app.py | $(OUT)
	$(PYTHON) tools/build_app.py $< $@

$(OUT)/docstats.bex: examples/c/docstats.c sdk/baseos.h sdk/baseos_abi.h sdk/start.c sdk/app.ld tools/build_app.py | $(OUT)
	$(PYTHON) tools/build_app.py $< $@

$(OUT)/pointer.bex: examples/c/pointer.c sdk/baseos.h sdk/baseos_abi.h sdk/start.c sdk/app.ld tools/build_app.py | $(OUT)
	$(PYTHON) tools/build_app.py $< $@

$(OUT)/sdk_examples.h: $(OUT)/hello-c.bex $(OUT)/notebook.bex $(OUT)/counter.bex $(OUT)/docstats.bex $(OUT)/pointer.bex tools/bin2c.py Makefile
	$(PYTHON) tools/bin2c.py $(OUT)/hello-c.bex sdk_hello > $@
	$(PYTHON) tools/bin2c.py $(OUT)/notebook.bex sdk_notebook >> $@
	$(PYTHON) tools/bin2c.py $(OUT)/counter.bex sdk_counter >> $@
	$(PYTHON) tools/bin2c.py $(OUT)/docstats.bex sdk_docstats >> $@
	$(PYTHON) tools/bin2c.py $(OUT)/pointer.bex sdk_pointer >> $@

$(OUT)/chime.wav: tools/make_audio_example.py | $(OUT)
	$(PYTHON) tools/make_audio_example.py $@

$(OUT)/audio_example.h: $(OUT)/chime.wav tools/bin2c.py
	$(PYTHON) tools/bin2c.py $< audio_example > $@

$(OUT)/build_info.h: FORCE tools/build_info.py | $(OUT)
	$(PYTHON) tools/build_info.py $(OUT)

FORCE:
.PHONY: FORCE

$(OUT)/kernel.o: $(OUT)/build_info.h $(OUT)/native_example.h $(OUT)/sdk_examples.h $(OUT)/audio_example.h

# Rendering is the hot path; retain size optimization for the rest of the kernel.
$(OUT)/gfx.o: CFLAGS := $(filter-out -Os,$(CFLAGS)) -O2

# Scalar audio/video decoder units use guarded x87; the desktop stays soft-float.
$(OUT)/media_mp3.o: CFLAGS := $(filter-out -msoft-float,$(CFLAGS)) -mhard-float -mfpmath=387 -Ithird_party/minimp3/compat
$(OUT)/media_mp3.o: third_party/minimp3/minimp3.h

# stb image decoding stays integer-only and uses a private bounded arena.
$(OUT)/image_decode.o: CFLAGS += -Ithird_party/stb/compat
$(OUT)/image_decode.o: third_party/stb/stb_image.h $(wildcard third_party/stb/compat/*.h)
$(OUT)/video.o: CFLAGS := $(filter-out -Os -msoft-float,$(CFLAGS)) -O2 -mhard-float -mfpmath=387 -Ithird_party/pl_mpeg/compat
$(OUT)/video.o: third_party/pl_mpeg/pl_mpeg.h

$(OUT)/kernel.elf: $(OBJS) $(OUT)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/kernel.packed: $(OUT)/kernel.bin tools/kernel_pack.py tools/layout.py $(SRC)/layout.h
	$(PYTHON) tools/kernel_pack.py $< $@

$(IMG): $(OUT)/boot.bin $(OUT)/kernel.packed tools/update_image.py tools/kernel_pack.py tools/layout.py $(SRC)/layout.h
	$(PYTHON) tools/update_image.py --packed $@ $(OUT)/boot.bin $(OUT)/kernel.packed

$(DATA_IMG): FORCE tools/init_data.py tools/layout.py $(SRC)/layout.h | $(OUT)
	$(PYTHON) tools/init_data.py $@ --profile $(DATA_PROFILE)

run: $(IMG) $(DATA_IMG)
	qemu-system-i386 -m $(QEMU_MEMORY) -vga std -nic user,model=rtl8139 $(QEMU_AUDIO) \
		-boot a -drive file=$(IMG),format=raw,index=0,if=floppy $(QEMU_DATA) \
		-serial file:$(OUT)/serial.out -no-reboot -display $(QEMU_DISPLAY)

headless: $(IMG) $(DATA_IMG)
	qemu-system-i386 -m $(QEMU_MEMORY) -vga std -nic user,model=rtl8139 $(QEMU_AUDIO) \
		-boot a -drive file=$(IMG),format=raw,index=0,if=floppy $(QEMU_DATA) \
		-serial stdio -display none -no-reboot

# Separate data filenames make selecting this profile non-destructive.
run-large:
	$(MAKE) run DATA_IMG=$(OUT)/baseos-large-data.img DATA_PROFILE=large QEMU_MEMORY=128M

headless-large:
	$(MAKE) headless DATA_IMG=$(OUT)/baseos-large-data.img DATA_PROFILE=large QEMU_MEMORY=128M

clean:
	rm -f $(OBJS) $(OUT)/boot.bin $(OUT)/kernel.bin $(OUT)/kernel.packed $(OUT)/kernel.elf $(OUT)/linker.ld $(OUT)/layout.inc

.PHONY: all run headless run-large headless-large clean

# Image and its backups intentionally survive clean.
test:
	$(PYTHON) -m unittest discover -s tests -p "test_*.py"

.PHONY: test
