# Clean Makefile for KernelPatch (Clang)
KP_DIR ?= ../KernelPatch
TARGET_COMPILE ?= aarch64-linux-gnu-
CC := clang
LD := ld.lld

SRC := src/layer1_hide.c
OUT := layer1_hide.kpm

INCLUDES := -I$(KP_DIR)/kernel/include \
	-I$(KP_DIR)/kernel/patch/include \
	-I$(KP_DIR)/kernel/linux/include \
	-I$(KP_DIR)/kernel/linux/arch/arm64/include

CFLAGS := -O2 -Wall -Wextra \
	-Wno-unused-parameter \
	-fno-stack-protector \
	-fno-pic -fno-pie \
	-nostdlib -ffreestanding \
	-fno-builtin -fno-common \
	-fno-strict-aliasing \
	-fno-omit-frame-pointer \
	-mgeneral-regs-only \
	-target aarch64-linux-android

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -r -o $@ $<
	@echo "[layer1-hide] built $@ with Clang"

clean:
	rm -f $(OUT) src/*.o src/*.ko
	@echo "[layer1-hide] cleaned"

.PHONY: all clean
