# FortressOS Build Automation
# Target: x86_64 UEFI Bare-Metal OS

SHELL := /bin/bash
.DELETE_ON_ERROR:

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
           -Isrc/kernel \
           -Isrc/lib \
           -Isrc/fs

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
BOOTABLE_IMG := $(BIN_DIR)/fortress.img

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

NVME_GPT_IMG := $(BUILD_DIR)/nvme_gpt.img
NVME_RAW_IMG := $(BUILD_DIR)/nvme_raw.img
NVME_IMG ?= $(NVME_GPT_IMG)

QEMU_NVME_FLAGS := -drive file=$(NVME_IMG),if=none,id=nvm0,format=raw -device nvme,serial=fortress0,drive=nvm0
QEMU_EXTRA ?=
ifeq ($(WRITE_TEST),1)
QEMU_EXTRA += -fw_cfg name=opt/fortress/write_test,string=1
endif
QEMU_FLAGS := -M q35 -m 2G -serial stdio $(QEMU_NVME_FLAGS) $(QEMU_EXTRA)

.DEFAULT_GOAL := all
.PHONY: all clean distclean run run-bios run-img run-img-bios run-img-usb debug limine-setup ovmf-setup iso img nvme-disk nvme-gpt-disk nvme-raw-disk

all: $(BOOTABLE_ISO) $(BOOTABLE_IMG)

.PHONY: test-ext2 test-ext2-write test-storage test-nmi test-boot-diagnostics test-console test-input test-shell test-power
test-input:
	@python3 scripts/test_input.py

test-power: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_power.py

test-shell: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_shell.py
	@python3 scripts/test_shell_no_uart.py
test-console:
	@python3 scripts/test_console.py

test-boot-diagnostics: $(BOOTABLE_ISO)
	@python3 scripts/test_boot_diagnostics.py

test-nmi: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_nmi_transitions.py

.PHONY: test-smp-percpu
test-smp-percpu: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_smp_percpu.py

.PHONY: test-smp-discovery
test-smp-discovery: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_smp_discovery.py

.PHONY: test-smp-locks
test-smp-locks: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_locks.py

.PHONY: test-smp-sched
test-smp-sched: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_sched.py

.PHONY: test-smp-ipi
test-smp-ipi: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_ipi.py

.PHONY: test-pmm-boot-host test-smp-memory-boot
test-pmm-boot-host:
	@python3 scripts/test_pmm_boot_host.py

test-smp-memory-boot: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_memory_boot.py

test-ext2:
	@python3 scripts/test_ext2.py

test-ext2-write: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_ext2_write.py

test-storage: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_storage_boot.py

.PHONY: test-usb-discovery
test-usb-discovery: $(BOOTABLE_ISO)
	@python3 scripts/test_usb_discovery.py

.PHONY: test-usb-reset
test-usb-reset: $(BOOTABLE_ISO)
	@python3 scripts/test_xhci_reset_host.py
	@python3 scripts/test_usb_discovery.py --reset

.PHONY: test-usb-rings
test-usb-rings: $(BOOTABLE_ISO)
	@python3 scripts/test_xhci_rings_host.py
	@python3 scripts/test_usb_discovery.py --rings

.PHONY: test-usb-ports
test-usb-ports: $(BOOTABLE_ISO)
	@python3 scripts/test_xhci_ports_host.py
	@python3 scripts/test_usb_discovery.py --ports

.PHONY: test-usb-descriptors
test-usb-descriptors: $(BOOTABLE_ISO)
	@python3 scripts/test_xhci_dev_host.py
	@python3 scripts/test_usb_discovery.py --descriptors

.PHONY: test-usb-block
test-usb-block: $(BOOTABLE_ISO) $(BOOTABLE_IMG)
	@python3 scripts/test_xhci_bot_host.py
	@python3 scripts/test_usb_discovery.py --block

.PHONY: test-usb-mount
test-usb-mount: $(BOOTABLE_ISO) $(BOOTABLE_IMG)
	@python3 scripts/test_usb_mount_host.py
	@python3 scripts/test_usb_discovery.py --mount

.PHONY: test-usb-persistence
test-usb-persistence: $(BOOTABLE_IMG)
	@python3 scripts/test_xhci_bot_host.py
	@python3 scripts/test_usb_mount_host.py
	@python3 scripts/test_usb_persistence.py




USER_DIR := user
USER_INIT_ELF := $(BUILD_DIR)/init.elf
USER_HELLO_ELF := $(BUILD_DIR)/hello.elf
USER_SHELL_ELF := $(BUILD_DIR)/shell.elf
INITRAMFS_TAR := $(BIN_DIR)/initramfs.tar

# Build user standalone init executable
$(USER_INIT_ELF): $(USER_DIR)/init.asm $(USER_DIR)/linker.ld
	@mkdir -p $(BUILD_DIR)
	@echo "  [AS]  $<"
	@$(AS) -f elf64 $< -o $(BUILD_DIR)/init.o
	@echo "  [LD]  $@"
	@$(LD) -m elf_x86_64 -nostdlib -static -T $(USER_DIR)/linker.ld $(BUILD_DIR)/init.o -o $@

# Build user standalone hello executable
$(USER_HELLO_ELF): $(USER_DIR)/hello.asm $(USER_DIR)/linker.ld
	@mkdir -p $(BUILD_DIR)
	@echo "  [AS]  $<"
	@$(AS) -f elf64 $< -o $(BUILD_DIR)/hello.o
	@echo "  [LD]  $@"
	@$(LD) -m elf_x86_64 -nostdlib -static -T $(USER_DIR)/linker.ld $(BUILD_DIR)/hello.o -o $@

# Freestanding user shell, separate address-space ELF (no host runtime).
$(USER_SHELL_ELF): $(USER_DIR)/shell.c $(USER_DIR)/shell_start.asm $(USER_DIR)/shell.ld src/fs/vfs.h src/include/types.h src/kernel/syscall.h src/arch/x86_64/idt.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $(USER_DIR)/shell.c -o $(BUILD_DIR)/shell.o
	@$(AS) -f elf64 $(USER_DIR)/shell_start.asm -o $(BUILD_DIR)/shell_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/shell_start.o $(BUILD_DIR)/shell.o -o $@

# Build USTAR Initramfs archive
$(INITRAMFS_TAR): $(USER_INIT_ELF) $(USER_HELLO_ELF) $(USER_SHELL_ELF) Makefile
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/etc $(BUILD_DIR)/initramfs/docs $(BIN_DIR)
	@cp -f $(USER_INIT_ELF) $(BUILD_DIR)/initramfs/bin/init
	@cp -f $(USER_SHELL_ELF) $(BUILD_DIR)/initramfs/bin/shell
	@cp -f $(USER_HELLO_ELF) $(BUILD_DIR)/initramfs/bin/hello
	@printf "========================================\n  Welcome to FortressOS (x86_64 UEFI)\n  Step 8B: Initramfs & VFS Active\n========================================\n" > $(BUILD_DIR)/initramfs/etc/motd
	@printf "FortressOS Documentation\nThe Ring 3 shell supports help, ls, cat and echo.\n" > $(BUILD_DIR)/initramfs/docs/readme.txt
	@echo "  [TAR] Generating USTAR archive $@"
	@tar --format=ustar -cf $(INITRAMFS_TAR) -C $(BUILD_DIR)/initramfs bin etc docs

# Compile C source files to object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Embedded init assembly depends explicitly on built user binary
$(BUILD_DIR)/kernel/embedded_init.o: $(SRC_DIR)/kernel/embedded_init.asm $(USER_INIT_ELF)
	@mkdir -p $(dir $@)
	@echo "  [AS]  $< (embedding $(USER_INIT_ELF))"
	@$(AS) $(ASFLAGS) $< -o $@

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

$(BOOTABLE_ISO): $(KERNEL_ELF) $(INITRAMFS_TAR) limine.conf limine-setup
	@echo "--> Preparing ISO filesystem hierarchy..."
	@mkdir -p $(ISO_ROOT)/boot/limine
	@mkdir -p $(ISO_ROOT)/EFI/BOOT
	@cp -f $(KERNEL_ELF) $(ISO_ROOT)/boot/fortress.elf
	@cp -f $(INITRAMFS_TAR) $(ISO_ROOT)/boot/initramfs.tar
	@cp -f $(INITRAMFS_TAR) $(ISO_ROOT)/initramfs.tar
	@cp -f limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	@cp -f limine.conf $(ISO_ROOT)/boot/limine.conf
	@if [ -f "assets/splash.png" ]; then \
		cp -f assets/splash.png $(ISO_ROOT)/boot/splash.png; \
		cp -f assets/splash.png $(ISO_ROOT)/boot/limine/splash.png; \
	fi
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

# Package bootable raw disk image (dual-boot GPT/ESP + persistent ext2)
img: $(BOOTABLE_IMG)

$(BOOTABLE_IMG): $(KERNEL_ELF) $(INITRAMFS_TAR) limine.conf limine-setup $(BOOTABLE_ISO)
	@mkdir -p $(BIN_DIR)
	@echo "--> Creating bootable raw disk image with scripts/create_boot_img.py..."
	@python3 scripts/create_boot_img.py $@ --iso-root $(ISO_ROOT) --limine-dir $(LIMINE_DIR)

# Create 32 MiB test GPT partitioned NVMe disk image
$(NVME_GPT_IMG): scripts/create_nvme_disk.py
	@mkdir -p $(BUILD_DIR)
	@python3 scripts/create_nvme_disk.py $@ --gpt

nvme-gpt-disk: $(NVME_GPT_IMG)
nvme-disk: $(NVME_GPT_IMG)

# Create 32 MiB test raw NVMe persistence disk image
$(NVME_RAW_IMG): scripts/create_nvme_disk.py
	@mkdir -p $(BUILD_DIR)
	@python3 scripts/create_nvme_disk.py $@ --raw

nvme-raw-disk: $(NVME_RAW_IMG)

# Launch operating system in QEMU under UEFI mode
run: $(BOOTABLE_ISO) $(NVME_IMG) ovmf-setup
	@echo "--> Launching FortressOS in QEMU (UEFI mode)..."
	@if [ -f "/usr/share/OVMF/OVMF_CODE_4M.fd" ] && [ -f "/usr/share/OVMF/OVMF_VARS_4M.fd" ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -f /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD_DIR)/OVMF_VARS.fd; \
		$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,unit=1,file=$(BUILD_DIR)/OVMF_VARS.fd -cdrom $(BOOTABLE_ISO); \
	elif [ -f "$(OVMF_FILE)" ]; then \
		$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FILE) -cdrom $(BOOTABLE_ISO); \
	else \
		echo "Warning: OVMF firmware not found, running BIOS mode fallback"; \
		$(QEMU) $(QEMU_FLAGS) -cdrom $(BOOTABLE_ISO); \
	fi

# Launch in QEMU under legacy BIOS mode
run-bios: $(BOOTABLE_ISO) $(NVME_IMG)
	@echo "--> Launching FortressOS in QEMU (BIOS mode)..."
	$(QEMU) $(QEMU_FLAGS) -boot d -cdrom $(BOOTABLE_ISO)

# Launch bootable raw disk image in QEMU under UEFI mode
run-img: $(BOOTABLE_IMG) $(NVME_IMG) ovmf-setup
	@echo "--> Launching FortressOS raw disk image in QEMU (UEFI mode)..."
	@if [ -f "/usr/share/OVMF/OVMF_CODE_4M.fd" ] && [ -f "/usr/share/OVMF/OVMF_VARS_4M.fd" ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -f /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD_DIR)/OVMF_VARS.fd; \
		$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,unit=1,file=$(BUILD_DIR)/OVMF_VARS.fd -drive file=$(BOOTABLE_IMG),if=none,id=bootdisk,format=raw -device ide-hd,drive=bootdisk,bootindex=1; \
	elif [ -f "$(OVMF_FILE)" ]; then \
		$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FILE) -drive file=$(BOOTABLE_IMG),if=none,id=bootdisk,format=raw -device ide-hd,drive=bootdisk,bootindex=1; \
	else \
		echo "Warning: OVMF firmware not found, running BIOS mode fallback"; \
		$(QEMU) $(QEMU_FLAGS) -drive file=$(BOOTABLE_IMG),format=raw; \
	fi

# Launch bootable raw disk image in QEMU as an emulated USB flash drive (UEFI mode)
run-img-usb: $(BOOTABLE_IMG) $(NVME_IMG) ovmf-setup
	@echo "--> Launching FortressOS raw disk image in QEMU (USB flash drive / UEFI mode)..."
	@if [ -f "/usr/share/OVMF/OVMF_CODE_4M.fd" ] && [ -f "/usr/share/OVMF/OVMF_VARS_4M.fd" ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -f /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD_DIR)/OVMF_VARS.fd; \
		$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,unit=1,file=$(BUILD_DIR)/OVMF_VARS.fd -device qemu-xhci -device usb-storage,drive=usbstick,bootindex=1 -drive file=$(BOOTABLE_IMG),if=none,id=usbstick,format=raw; \
	elif [ -f "$(OVMF_FILE)" ]; then \
		$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FILE) -device qemu-xhci -device usb-storage,drive=usbstick,bootindex=1 -drive file=$(BOOTABLE_IMG),if=none,id=usbstick,format=raw; \
	else \
		echo "Warning: OVMF firmware not found, running BIOS mode fallback"; \
		$(QEMU) $(QEMU_FLAGS) -drive file=$(BOOTABLE_IMG),format=raw; \
	fi

# Launch bootable raw disk image in QEMU under legacy BIOS mode
run-img-bios: $(BOOTABLE_IMG) $(NVME_IMG)
	@echo "--> Launching FortressOS raw disk image in QEMU (BIOS mode)..."
	$(QEMU) $(QEMU_FLAGS) -drive file=$(BOOTABLE_IMG),format=raw

# Launch with GDB debugging stub enabled
debug: $(BOOTABLE_ISO) $(NVME_IMG) ovmf-setup
	@echo "--> Launching FortressOS with GDB debugging enabled (target remote :1234)..."
	@if [ -f "/usr/share/OVMF/OVMF_CODE_4M.fd" ] && [ -f "/usr/share/OVMF/OVMF_VARS_4M.fd" ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -f /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD_DIR)/OVMF_VARS.fd; \
		$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,unit=1,file=$(BUILD_DIR)/OVMF_VARS.fd -cdrom $(BOOTABLE_ISO) -s -S; \
	elif [ -f "$(OVMF_FILE)" ]; then \
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

# Phase 6B: PMM SMP Memory Test Targets
# Build and run the PMM SMP host test harness
BIN_DIR ?= bin
TEST_HOST_BIN := $(BIN_DIR)/pmm_smp_test
TEST_HOST_SRCS := tests/pmm_smp_test.c src/mm/pmm.c src/mm/pmm_host_shim.c
TEST_HOST_CFLAGS := $(CFLAGS) -DTEST_SMP_MEMORY -pthread

$(TEST_HOST_BIN): $(TEST_HOST_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_HOST_CFLAGS) -o $@ $^

.PHONY: test-smp-memory-host

test-smp-memory-host: $(TEST_HOST_BIN)
	@echo "--- Running PMM SMP memory host test ---"
	@$(TEST_HOST_BIN)

# Placeholder for freestanding QEMU SMP memory test (not yet implemented)
.PHONY: test-smp-memory

test-smp-memory:
	@echo "Freestanding SMP memory test not yet implemented"
