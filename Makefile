CROSS   := aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

BUILD   := build
SRC     := src
XZ      := xz-embedded
FDT     := libfdt

GCC_INCLUDES := $(shell $(CC) -print-file-name=include)

CFLAGS := \
    -Wall -Wextra -O2 \
    -ffreestanding -nostdlib -nostdinc \
    -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie \
    -march=armv8-a \
    -I$(GCC_INCLUDES) \
    -I$(SRC) -I$(XZ) -I$(FDT)

ASFLAGS := -march=armv8-a -I$(SRC) -I$(XZ)

OBJS := \
    $(BUILD)/start.o         \
    $(BUILD)/str.o           \
    $(BUILD)/loader.o        \
    $(BUILD)/xz_crc32.o      \
    $(BUILD)/xz_dec_lzma2.o  \
    $(BUILD)/xz_dec_stream.o \
    $(BUILD)/fdt.o           \
    $(BUILD)/fdt_ro.o        \
    $(BUILD)/fdt_rw.o        \
    $(BUILD)/fdt_wip.o       \
    $(BUILD)/blobs.o

.PHONY: all clean

all: $(BUILD)/loader.elf

$(BUILD):
	mkdir -p $(BUILD)

# --- compile device tree ---
$(BUILD)/CCR2004-dt-1.dtb: src/CCR2004-1G-2XS-PCIe.dts | $(BUILD)
	@echo "[DTC] Compiling device tree..."
	dtc -I dts -O dtb -o $@ $<
	@echo "      DTB = $$(wc -c < $@) bytes"

# --- compress kernel with xz ---
$(BUILD)/kernel.xz: resources/Image | $(BUILD)
	@echo "[XZ]  Compressing kernel..."
	xz --check=crc32 -9e -c $< > $@
	@echo "      kernel.xz = $$(wc -c < $@) bytes"

# --- compile ---
$(BUILD)/start.o: $(SRC)/start.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/str.o: $(SRC)/str.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/loader.o: $(SRC)/loader.c $(SRC)/uart.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/xz_crc32.o: $(XZ)/xz_crc32.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/xz_dec_lzma2.o: $(XZ)/xz_dec_lzma2.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/xz_dec_stream.o: $(XZ)/xz_dec_stream.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt.o: $(FDT)/fdt.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt_ro.o: $(FDT)/fdt_ro.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt_rw.o: $(FDT)/fdt_rw.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt_wip.o: $(FDT)/fdt_wip.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/blobs.o: $(SRC)/blobs.S $(BUILD)/kernel.xz $(BUILD)/CCR2004-dt-1.dtb | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# --- link ---
$(BUILD)/loader.elf: $(OBJS) $(SRC)/linker.ld
	$(LD) -T $(SRC)/linker.ld -o $@ $(OBJS)
	@echo ""
	@echo "=== loader.elf ==="
	@ls -lh $@
	@$(SIZE) $@
	@echo ""
	@$(OBJDUMP) -h $@ | grep -E '^\s+[0-9]|Name'

clean:
	rm -rf $(BUILD)
