# layer1-hide Makefile
# Builds layer1_hide.kpm with no local PC needed, via GitHub Actions.
# Usage:
#   make KP_DIR=../KernelPatch TARGET_COMPILE=aarch64-linux-gnu-
#   make clean

KP_DIR ?= ../KernelPatch
TARGET_COMPILE ?= aarch64-linux-gnu-

CC := $(TARGET_COMPILE)gcc
SRC := src/layer1_hide.c
OUT := layer1_hide.kpm
OUT_ALIAS := layer1-hide.kpm

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
	-march=armv8-a -mtune=generic

all: $(OUT) $(OUT_ALIAS)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -r -o $@ $<
	@echo "[layer1-hide] built $@"

$(OUT_ALIAS): $(OUT)
	cp -f $(OUT) $(OUT_ALIAS)
	@echo "[layer1-hide] copied to $(OUT_ALIAS)"

clean:
	rm -f $(OUT) $(OUT_ALIAS) src/*.o src/*.ko
	@echo "[layer1-hide] cleaned"

.PHONY: all clean
