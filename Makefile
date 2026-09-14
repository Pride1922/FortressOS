# FortressOS Build Automation
# Target: x86_64 UEFI Bare-Metal OS

SHELL := /bin/bash

# Toolchain configuration
CC      ?= gcc
LD      ?= ld
AS      := nasm
QEMU    ?= qemu-system-x86_64
XORRISO ?= xorriso
GIT     ?= git

# Strict freestanding compilation flags
CFLAGS  := -std=c11 \
           -ffreestanding \
           -fno-stack-protector \
           -fno-stack-check \
           -fno-lto \
           -fPIE \
           -m64 \
           -march=x86-64 \
           -mno-80387 \
           -mno-mmx \
           -mno-sse \
           -mno-sse2 \
           -mno-red-zone \
           -Wall \
           -Wextra \
           -Werror \
           -g \
           -MMD \
           -MP \
           -Isrc/include \
           -Isrc/drivers \
           -Isrc/arch/x86_64 \
           -Isrc/mm \
           -Isrc/lib

# Assembler flags for NASM (with DWARF debugging symbols)
ASFLAGS := -f elf64 -g -F dwarf

# Linker flags for higher-half 64-bit ELF kernel
LDFLAGS := -m elf_x86_64 \
           -nostdlib \
           -static \
           -pie \
           --no-dynamic-linker \
           -z text \
           -z noexecstack \
           -z max-page-size=0x1000 \
           -T linker.ld

# Directories
SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin
ISO_ROOT  := $(BUILD_DIR)/iso_root

# Target binary artifacts
KERNEL_ELF := $(BIN_DIR)/fortress.elf
BOOTABLE_ISO := $(BIN_DIR)/fortress.iso

# Source files
C_SRCS   := $(shell find $(SRC_DIR) -type f -name '*.c')
ASM_SRCS := $(shell find $(SRC_DIR) -type f -name '*.asm')

# Object files
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS)) \
        $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))

# Header dependency files (.d)
DEPS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.d, $(C_SRCS))
-include $(DEPS)

# Limine bootloader branch and repository
LIMINE_BRANCH := v8.x-binary
LIMINE_DIR    := limine

# OVMF Firmware lookup
OVMF_PATHS := /usr/share/ovmf/OVMF.fd \
              /usr/share/qemu/OVMF.fd \
              /usr/share/OVMF/OVMF_CODE_4M.fd \
              /usr/share/OVMF/OVMF_CODE.fd \
              /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
              ovmf/OVMF_CODE.fd \
              ovmf/OVMF.fd

OVMF_FILE := $(firstword $(wildcard $(OVMF_PATHS)))
ifeq ($(OVMF_FILE),)
    OVMF_FILE := ovmf/OVMF.fd
endif

QEMU_FLAGS := -M q35 -m 2G -serial stdio

.PHONY: all clean distclean run run-bios debug limine-setup ovmf-setup iso

all: $(BOOTABLE_ISO)

# Compile C source files to object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Assemble NASM assembly files to object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	@echo "  [AS]  $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Link kernel ELF binary
$(KERNEL_ELF): $(OBJS) linker.ld
	@mkdir -p $(BIN_DIR)
	@echo "  [LD]  $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

# Ensure Limine bootloader binaries are cloned and built
limine-setup:
	@if [ ! -d "$(LIMINE_DIR)" ]; then \
		echo "--> Cloning Limine ($(LIMINE_BRANCH))..."; \
		$(GIT) clone https://github.com/limine-bootloader/limine.git --branch=$(LIMINE_BRANCH) --depth=1 $(LIMINE_DIR); \
	fi
	@if [ ! -f "$(LIMINE_DIR)/limine" ]; then \
		echo "--> Building Limine host tool..."; \
		$(MAKE) -C $(LIMINE_DIR); \
	fi

# Download OVMF UEFI firmware if not available in host system
ovmf-setup:
	@if [ ! -f "$(OVMF_FILE)" ]; then \
		echo "--> Setting up local OVMF UEFI firmware..."; \
		mkdir -p ovmf; \
		if command -v curl >/dev/null 2>&1; then \
			curl -sL https://github.com/rust-osdev/ovmf-prebuilt/releases/latest/download/x64-code.fd -o ovmf/OVMF.fd || true; \
		fi; \
	fi

# Package bootable ISO image
iso: $(BOOTABLE_ISO)

$(BOOTABLE_ISO): $(KERNEL_ELF) limine.conf limine-setup
	@echo "--> Preparing ISO filesystem hierarchy..."
	@mkdir -p $(ISO_ROOT)/boot/limine
	@mkdir -p $(ISO_ROOT)/EFI/BOOT
	@cp -f $(KERNEL_ELF) $(ISO_ROOT)/boot/fortress.elf
	@cp -f limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	@cp -f limine.conf $(ISO_ROOT)/boot/limine.conf
	@cp -f $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/boot/limine/ 2>/dev/null || true
	@cp -f $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/boot/limine/ 2>/dev/null || true
	@cp -f $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/ 2>/dev/null || true
	@cp -f $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/ 2>/dev/null || true
	@cp -f $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/ 2>/dev/null || true
	@echo "--> Creating bootable ISO with xorriso..."
	@$(XORRISO) -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(BOOTABLE_ISO)
	@echo "--> Deploying Limine BIOS boot record to ISO..."
	@$(LIMINE_DIR)/limine bios-install $(BOOTABLE_ISO) 2>/dev/null || true
	@echo "[OK] Bootable ISO generated: $(BOOTABLE_ISO)"

# Launch operating system in QEMU under UEFI mode
run: $(BOOTABLE_ISO) ovmf-setup
	@echo "--> Launching FortressOS in QEMU (UEFI mode)..."
	@if [ -f "$(OVMF_FILE)" ]; then \
		$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FILE) -cdrom $(BOOTABLE_ISO); \
	else \
		echo "Warning: OVMF firmware not found, running BIOS mode fallback"; \
		$(QEMU) $(QEMU_FLAGS) -cdrom $(BOOTABLE_ISO); \
	fi

# Launch in QEMU under legacy BIOS mode
run-bios: $(BOOTABLE_ISO)
	@echo "--> Launching FortressOS in QEMU (BIOS mode)..."
	$(QEMU) $(QEMU_FLAGS) -cdrom $(BOOTABLE_ISO)

# Launch with GDB debugging stub enabled
debug: $(BOOTABLE_ISO) ovmf-setup
	@echo "--> Launching FortressOS with GDB debugging enabled (target remote :1234)..."
	@if [ -f "$(OVMF_FILE)" ]; then \
		$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FILE) -cdrom $(BOOTABLE_ISO) -s -S; \
	else \
		$(QEMU) $(QEMU_FLAGS) -cdrom $(BOOTABLE_ISO) -s -S; \
	fi

# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "[OK] Build directory cleaned."

# Clean build artifacts and external repositories
distclean: clean
	@rm -rf $(LIMINE_DIR) ovmf
	@echo "[OK] Project distclean complete."
