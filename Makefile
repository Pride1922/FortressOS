# FortressOS Build Automation
# Target: x86_64 UEFI Bare-Metal OS

# Phase 0 only: isolated installed NVMe fixtures, BIOS/UEFI x SMP=1/4/8.
# Production networking/spawn sources are never patched or built in place.
.PHONY: test-network-smp-phase0
test-network-smp-phase0:
	@python3 scripts/test_network_smp_phase0.py

SHELL := /bin/bash
.PHONY: test-network-smp-phase3-host test-network-smp-phase3
test-network-smp-phase3-host:
	@python3 scripts/test_network_smp_phase3_host.py
test-network-smp-phase3: test-network-smp-phase3-host
	@python3 scripts/test_network_smp_phase3.py
.PHONY: verify-network-smp-phase3-release
verify-network-smp-phase3-release:
	@python3 scripts/verify_network_smp_phase3_release.py
.PHONY: test-network-smp-phase1-host
.PHONY: test-network-smp-phase1
test-network-smp-phase1: test-network-smp-phase1-host
	@python3 scripts/test_network_smp_phase1.py
test-network-smp-phase1-host:
	@python3 scripts/test_network_smp_phase1_host.py
.DELETE_ON_ERROR:
.PHONY: test-install-provision-host
test-install-provision-host:
	@python3 scripts/test_install_provision.py
.PHONY: test-install-fat-host
test-install-fat-host:
	@python3 scripts/test_install_fat.py
.PHONY: test-install-control-host
test-install-control-host:
	@python3 scripts/test_install_control.py

.PHONY: test-install-payload-host
test-install-payload-host:
	@python3 scripts/test_install_payload.py

.PHONY: test-account-redo-host test-accounts-persistent-host test-accounts-persistent
test-account-redo-host:
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -Isrc/include tests/account_redo_host.c src/lib/account_redo.c user/tools/digest.c -o build/account-redo-host
	@build/account-redo-host
test-accounts-persistent-host:
	@python3 scripts/test_root_phase3.py --host-only
test-accounts-persistent:
	@python3 scripts/test_root_phase3.py

.PHONY: test-resched-return-host


test-resched-return-host:
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -Isrc/include -Isrc/kernel -Isrc/arch/x86_64 tests/resched_return_host.c -o build/resched-return-host
	@build/resched-return-host

.PHONY: test-wait-profile-host
test-wait-profile-host: test-smpbench-profile-host
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -Isrc/include -Isrc/kernel tests/wait_profile_host.c -o build/wait-profile-host
	@build/wait-profile-host

.PHONY: test-elf-page-host
test-elf-page-host:
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -Isrc/include -Isrc/kernel tests/elf_page_host.c -o build/elf-page-host
	@build/elf-page-host
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -Isrc/include -Isrc/kernel -Isrc/mm -Isrc/drivers -Isrc/arch/x86_64 -DFORTRESS_THREAD_H -include tests/host/thread.h tests/elf_loader_host.c src/kernel/elf.c -o build/elf-loader-host
	@build/elf-loader-host

# Owned isolated pause build only; never builds or attaches shared data disks.
.PHONY: test-ext4-physical-commit-pause
test-ext4-physical-commit-pause:
	@test -n "$(PHYSICAL_WORKSPACE)" || { echo 'Set PHYSICAL_WORKSPACE to the prepared disposable pause build'; exit 1; }
	@python3 scripts/test_ext4_physical_commit_pause.py "$(PHYSICAL_WORKSPACE)"

# Toolchain configuration
.PHONY: test-ext4-physical-open-unlink
test-ext4-physical-open-unlink:
	@test -n "$(PHYSICAL_WORKSPACE)" -a -n "$(PHYSICAL_ARTIFACT)" || { echo 'Set PHYSICAL_WORKSPACE and PHYSICAL_ARTIFACT to the prepared disposable case'; exit 1; }
	@python3 scripts/test_ext4_physical_open_unlink.py "$(PHYSICAL_WORKSPACE)" "$(PHYSICAL_ARTIFACT)"

# Toolchain configuration
CC      ?= gcc
LD      ?= ld
AS      := nasm
QEMU    ?= qemu-system-x86_64
XORRISO ?= xorriso
GIT     ?= git
SMP     ?= 1
BUILD_GIT_HASH ?= $(shell $(GIT) rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE     ?= $(shell date -u +%Y-%m-%d 2>/dev/null || echo "unknown")

# Strict freestanding compilation flags
CFLAGS  := -std=c11 -DFORTRESS_DAC_ENFORCED \
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
           -Isrc/fs \
           -Isrc/net

# Assembler flags for NASM (with DWARF debugging symbols)
ASFLAGS := -f elf64 -g -F dwarf

# Explicit isolated test builds only. Normal builds have no permission logging.
ifeq ($(PERMISSIONS_TRACE),1)
CFLAGS += -DFORTRESS_PERMISSIONS_TRACE
endif
ifeq ($(PERMISSIONS_TEST),1)
CFLAGS += -DTEST_PERMISSIONS_ENFORCEMENT
endif

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
C_SRCS   := $(filter-out src/lib/crypto/% src/lib/tls/%,$(shell find $(SRC_DIR) -type f -name '*.c'))
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
.PHONY: test-root-boot-host test-root-phase2-host test-root-phase2
test-root-boot-host:
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/host -Isrc/include -Isrc/lib -Isrc/fs -Isrc/kernel -Isrc/mm -Isrc/drivers tests/root_boot_host.c src/kernel/root_boot.c src/kernel/boot_info.c src/fs/gpt.c user/tools/userdb.c user/tools/digest.c -o build/root-boot-host
	@build/root-boot-host
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/host -Isrc/include -Isrc/mm -Isrc/drivers tests/pci_inventory_host.c -o build/pci-inventory-host
	@build/pci-inventory-host
	@gcc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -no-pie -DTOOL_HOST_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections -Isrc/include -Isrc/fs tests/recovery_host.c user/tools/common.c user/tools/digest.c -lcrypt -o build/recovery-host
	@build/recovery-host
test-root-phase2-host:
	@python3 scripts/test_root_phase2.py --host-only
test-root-phase2:
	@python3 scripts/test_root_phase2.py
.PHONY: all live clean distclean run run-bios run-img run-img-bios run-img-usb debug limine-setup ovmf-setup iso img nvme-disk nvme-gpt-disk nvme-raw-disk

all: $(BOOTABLE_ISO) $(BOOTABLE_IMG)
live: all
include src/lib/crypto/crypto.mk
include src/lib/tls/tls.mk

# C_SRCS/OBJS above automatically include src/fs/pipe.c.
.PHONY: test-s8-process-host test-s8-process
test-s8-process-host:
	@python3 scripts/test_process_table_host.py

.PHONY: test-s8-groups-host
test-s9-metadata-host:
	@python3 scripts/test_process_table_host.py --metadata

.PHONY: test-s9-metadata
test-s9-metadata: $(BOOTABLE_ISO)
	@python3 scripts/test_s9_metadata.py

.PHONY: test-s9-sysinfo-host
test-s9-sysinfo-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -Wall -Wextra -Werror -Isrc/include tests/sysinfo_host.c -o $(BUILD_DIR)/sysinfo_host
	@$(BUILD_DIR)/sysinfo_host

.PHONY: test-s9-sysinfo
test-s9-sysinfo: $(BOOTABLE_ISO)
	@python3 scripts/test_s9_sysinfo.py

.PHONY: test-s9-top-host
test-s9-top-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/top_host.c -o $(BUILD_DIR)/top_host
	@$(BUILD_DIR)/top_host

.PHONY: test-s9-top
test-s9-top: $(BOOTABLE_ISO)
	@python3 scripts/test_s9_top.py

.PHONY: test-nano-host
test-nano-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Iuser tests/nano_host.c -o $(BUILD_DIR)/nano_host
	@$(BUILD_DIR)/nano_host

.PHONY: test-nano
test-nano: $(BOOTABLE_ISO)
	@python3 scripts/test_nano.py


.PHONY: test-s8-groups-host
	@python3 scripts/test_process_table_host.py --groups

.PHONY: test-s8-terminal-host test-s8-terminal
test-s8-terminal-host:
	@python3 scripts/test_s8_terminal_host.py

$(BUILD_DIR)/s8_terminal_user.elf: tests/s8_terminal_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/terminal.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_terminal_user.c -o $(BUILD_DIR)/s8_terminal_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_terminal_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_terminal_user_start.o $(BUILD_DIR)/s8_terminal_user.o -o $@

test-s8-terminal: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_terminal_user.elf
	@SMP=$(SMP) python3 scripts/test_s8_terminal.py

.PHONY: test-s8-signals-host test-s8-signals
test-s8-signals-host:
	@python3 scripts/test_process_table_host.py --signals

$(BUILD_DIR)/s8_signal_user.elf: tests/s8_signal_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_signal_user.c -o $(BUILD_DIR)/s8_signal_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_signal_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_signal_user_start.o $(BUILD_DIR)/s8_signal_user.o -o $@

test-s8-signals: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_signal_user.elf
	@python3 scripts/test_s8_process.py --signals

.PHONY: test-s8-sigpipe-host test-s8-sigpipe
test-s8-sigpipe-host: test-pipe-host test-s8-signals-host test-stream-tools-host test-runner-host

$(BUILD_DIR)/s8_sigpipe_user.elf: tests/s8_sigpipe_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/fs/vfs.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_sigpipe_user.c -o $(BUILD_DIR)/s8_sigpipe_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_sigpipe_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_sigpipe_user_start.o $(BUILD_DIR)/s8_sigpipe_user.o -o $@

test-s8-sigpipe: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_sigpipe_user.elf
	@SMP=$(SMP) python3 scripts/test_s8_sigpipe.py

.PHONY: test-s8-stops-host test-s8-stops
test-s8-stops-host:
	@python3 scripts/test_process_table_host.py --stops

.PHONY: test-s8-jobs-host
test-s8-jobs-host:
	@python3 scripts/test_s8_jobs_host.py

$(BUILD_DIR)/s8_stop_user.elf: tests/s8_stop_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_stop_user.c -o $(BUILD_DIR)/s8_stop_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_stop_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_stop_user_start.o $(BUILD_DIR)/s8_stop_user.o -o $@

test-s8-stops: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_stop_user.elf
	@python3 scripts/test_s8_process.py --stops

$(BUILD_DIR)/s8_jobs_user.elf: tests/s8_jobs_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/terminal.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_jobs_user.c -o $(BUILD_DIR)/s8_jobs_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_jobs_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_jobs_user_start.o $(BUILD_DIR)/s8_jobs_user.o -o $@

.PHONY: test-s8-jobs
test-s8-jobs: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_jobs_user.elf
	@SMP=$(SMP) python3 scripts/test_s8_jobs.py

$(BUILD_DIR)/s8_jobs_delay_user.elf: tests/s8_jobs_delay_user.c user/shell_start.asm user/shell.ld src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_jobs_delay_user.c -o $(BUILD_DIR)/s8_jobs_delay_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_jobs_delay_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_jobs_delay_user_start.o $(BUILD_DIR)/s8_jobs_delay_user.o -o $@

.PHONY: test-s8-jobs-idle
test-s8-jobs-idle: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_jobs_delay_user.elf
	@SMP=$(SMP) python3 scripts/test_s8_jobs_idle.py

$(BUILD_DIR)/s8_process_user.elf: tests/s8_process_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_process_user.c -o $(BUILD_DIR)/s8_process_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_process_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_process_user_start.o $(BUILD_DIR)/s8_process_user.o -o $@

test-s8-process: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_process_user.elf
	@python3 scripts/test_s8_process.py

.PHONY: test-pipe-host test-host
test-pipe-host:
	@python3 scripts/test_pipe_host.py

test-host: test-pipe-host test-shell-host test-ext2 test-stream-tools-host

$(BUILD_DIR)/pipe_user.elf: tests/pipe_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/fs/vfs.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/pipe_user.c -o $(BUILD_DIR)/pipe_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/pipe_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/pipe_user_start.o $(BUILD_DIR)/pipe_user.o -o $@

.PHONY: test-pipe
test-pipe: $(BOOTABLE_ISO) $(BUILD_DIR)/pipe_user.elf
	@python3 scripts/test_pipe.py

.PHONY: test-ext2 test-ext2-write test-storage test-nmi test-boot-diagnostics test-console test-input test-shell test-power
test-input:
	@python3 scripts/test_input.py

.PHONY: test-shell-host test-shell-integration test-shell-s3-s4
test-shell-host: test-input test-console
	@python3 scripts/test_shell_host.py
	@python3 scripts/test_pipeline_host.py
	@python3 scripts/test_shell_prompt_host.py
	@python3 scripts/test_shell_identity_host.py
	@python3 scripts/test_runner_host.py

.PHONY: test-runner-host
test-runner-host:
	@python3 scripts/test_runner_host.py

.PHONY: test-builtin-host
test-builtin-host: test-pipeline-host test-runner-host

.PHONY: test-pipeline-host
test-pipeline-host:
	@python3 scripts/test_pipeline_host.py
test-shell-s3-s4: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_shell_s3_s4.py
test-shell-integration: test-shell

test-power: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_power.py

test-shell: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_shell.py
	@python3 scripts/test_shell_no_uart.py
	@python3 scripts/test_shell_s3_s4.py
test-console:
	@python3 scripts/test_console.py

test-boot-diagnostics: $(BOOTABLE_ISO)
	@python3 scripts/test_boot_diagnostics.py

$(BUILD_DIR)/s8_nmi_user.elf: tests/s8_nmi_user.c tests/s8_nmi_context.asm user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s8_nmi_user.c -o $(BUILD_DIR)/s8_nmi_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_nmi_start.o
	@$(AS) -f elf64 tests/s8_nmi_context.asm -o $(BUILD_DIR)/s8_nmi_context.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_nmi_start.o $(BUILD_DIR)/s8_nmi_context.o $(BUILD_DIR)/s8_nmi_user.o -o $@

test-nmi: $(BOOTABLE_ISO) $(NVME_GPT_IMG) $(BUILD_DIR)/s8_nmi_user.elf
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

.PHONY: test-pmm-boot-host test-smp-memory-boot test-vmm-host
test-pmm-boot-host:
	@python3 scripts/test_pmm_boot_host.py

test-vmm-host:
	@python3 scripts/test_vmm_space_host.py

.PHONY: test-heap-memory-host test-ext4-memory-churn-host test-memory-churn test-memory-nodes
test-memory-nodes: all
	@python3 scripts/test_ext4_memory_churn_host.py
	@python3 scripts/test_ext4_memory_churn_host.py --api
	@python3 scripts/test_memory_node_mount.py
	@python3 scripts/test_memory_churn.py

.PHONY: test-memory-pressure
test-memory-pressure: all
	@python3 scripts/test_memory_pressure.py

.PHONY: test-memory-burst
test-memory-burst: all
	@python3 scripts/test_memory_burst.py

.PHONY: test-memory-cohort
test-memory-cohort: all
	@python3 scripts/test_memory_cohort.py

.PHONY: test-memory-rollback
test-memory-rollback: all
	@python3 scripts/test_memory_rollback.py

.PHONY: test-memory-observability
test-memory-observability: all
	@python3 scripts/test_memory_observability.py

.PHONY: test-smp-memory-profile
test-smp-memory-profile: all
	@python3 scripts/test_smp_memory_profile.py

test-memory-churn: all
	@python3 scripts/test_memory_churn.py

test-ext4-memory-churn-host: $(BOOTABLE_IMG)
	@python3 scripts/test_ext4_memory_churn_host.py

test-heap-memory-host:
	@python3 scripts/test_heap_memory_host.py

.PHONY: test-pmm-audit-host
test-pmm-audit-host:
	@python3 scripts/test_pmm_audit_host.py

.PHONY: test-memory
test-memory:
	@python3 scripts/test_memory_investigation.py

.PHONY: test-kstack-batch-host
test-kstack-batch-host: test-vmm-host
	@python3 scripts/test_kstack_batch_host.py

test-smp-memory-boot: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_memory_boot.py

.PHONY: test-smp-vmm
test-smp-vmm: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_vmm.py

.PHONY: capture-smp-panic
# BIOS/TCG, ISO only; diagnostic capture, not a performance or correctness gate.
capture-smp-panic: $(BOOTABLE_ISO) scripts/capture_smp_panic.py
	@python3 scripts/capture_smp_panic.py --iso bin/fortress.iso --elf bin/fortress.elf --cpus $(SMP) --runs 5 --reps 7 --dump-ram --output build/panic-capture-$$(date -u +%Y%m%d-%H%M%S)

.PHONY: test-smpbench-barrier-host test-smpbench-barrier
test-smpbench-barrier-host:
	@python3 scripts/test_smpbench_barrier_host.py

test-smpbench-barrier: $(BOOTABLE_ISO) test-smpbench-barrier-host
	@python3 scripts/test_smpbench_barrier.py

.PHONY: test-smpbench-profile-host test-smpbench-profile
test-smpbench-profile-host: test-smpbench-barrier-host
	@python3 scripts/test_smp_profile_parser.py

test-smpbench-profile: $(BOOTABLE_ISO) test-smpbench-profile-host
	@python3 scripts/test_smpbench_barrier.py --profile

.PHONY: prepare-dell-smpbench
prepare-dell-smpbench: $(BOOTABLE_IMG)
	@python3 scripts/prepare_dell_smpbench.py --output build/dell-smpbench-$$(date -u +%Y%m%d-%H%M%S)

.PHONY: prepare-kstack-comparison
.PHONY: prepare-elf-copy-comparison
prepare-elf-copy-comparison: $(BOOTABLE_IMG)
	@python3 scripts/prepare_kstack_comparison.py --experiment elf-copy --output build/elf-copy-ab-$$(date -u +%Y%m%d-%H%M%S)

.PHONY: prepare-pipe-wait-comparison
prepare-pipe-wait-comparison: $(BOOTABLE_IMG)
	@python3 scripts/prepare_kstack_comparison.py --experiment elf-copy --pipe-waits --output build/pipe-wait-ab-$$(date -u +%Y%m%d-%H%M%S)

.PHONY: prepare-resched-comparison
prepare-resched-comparison: $(BOOTABLE_IMG)
	@python3 scripts/prepare_kstack_comparison.py --experiment resched --pipe-waits --output build/resched-ab-$$(date -u +%Y%m%d-%H%M%S)

.PHONY: prepare-resched-policy-comparison
prepare-resched-policy-comparison: $(BOOTABLE_IMG)
	@python3 scripts/prepare_kstack_comparison.py --experiment resched-policy --pipe-waits --output build/resched-policy-ab-$$(date -u +%Y%m%d-%H%M%S)

prepare-kstack-comparison: $(BOOTABLE_IMG)
	@python3 scripts/prepare_kstack_comparison.py --output build/kstack-ab-$$(date -u +%Y%m%d-%H%M%S)

test-ext2:
	@python3 scripts/test_ext2.py

test-ext2-write: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_ext2_write.py

.PHONY: test-smp-append
test-smp-append: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_smp_append.py

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
.PHONY: test-xhci-bot-host test-usb-mount-host
test-xhci-bot-host:
	@python3 scripts/test_xhci_bot_host.py
test-usb-mount-host:
	@python3 scripts/test_usb_mount_host.py
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

.PHONY: test-hub-host test-hub-host-tsan test-usb-hid test-usb-hub test-usb-mouse test-rtl8153-host test-wlan-crypto-host test-wlan-mac-host test-wlan-wpa2-host test-wifi-host test-wifi-fault-host test-wifi-stack
WIFI_STACK_BUDGET ?= 1024
WIFI_STACK_SRCS := src/net/wlan/wlan_mgr.c src/net/wlan/net_80211.c src/net/wlan/wlan_wpa2.c src/net/wlan/wlan_crypto.c src/drivers/iwlwifi.c src/drivers/iwl_health.c
test-wifi-stack:
	@python3 scripts/check_wifi_stack.py $(WIFI_STACK_BUDGET) $(CC) $(BUILD_DIR)/stackgate $(CFLAGS) -- $(WIFI_STACK_SRCS)
test-wifi-fault-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -DTEST_SMP_MEMORY -DIWLWIFI_HOST_TEST -Isrc/include -Isrc/net/wlan -Isrc/drivers -Isrc/kernel -Isrc/fs -Isrc/mm tests/wifi_fault_host.c src/net/wlan/wlan_crypto.c src/net/wlan/net_80211.c src/net/wlan/wlan_wpa2.c src/net/wlan/wlan_mgr.c src/drivers/iwl_health.c -o $(BUILD_DIR)/wifi_fault_host
	@$(BUILD_DIR)/wifi_fault_host
test-wifi-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -DTEST_SMP_MEMORY -DIWLWIFI_HOST_TEST -Isrc/include -Isrc/net/wlan -Isrc/drivers -Isrc/kernel -Isrc/fs -Isrc/mm tests/wifi_host.c src/net/wlan/wlan_crypto.c src/net/wlan/net_80211.c src/net/wlan/wlan_wpa2.c src/net/wlan/wlan_mgr.c src/drivers/iwlwifi.c src/drivers/iwl_health.c -o $(BUILD_DIR)/wifi_host
	@$(BUILD_DIR)/wifi_host
test-wlan-wpa2-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/net/wlan tests/wlan_wpa2_host.c src/net/wlan/wlan_crypto.c src/net/wlan/wlan_wpa2.c -o $(BUILD_DIR)/wlan_wpa2_host
	@$(BUILD_DIR)/wlan_wpa2_host

test-wlan-mac-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/net/wlan tests/wlan_mac_host.c src/net/wlan/net_80211.c -o $(BUILD_DIR)/wlan_mac_host
	@$(BUILD_DIR)/wlan_mac_host

test-wlan-crypto-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/net/wlan tests/wlan_crypto_host.c src/net/wlan/wlan_crypto.c -o $(BUILD_DIR)/wlan_crypto_host
	@$(BUILD_DIR)/wlan_crypto_host

test-rtl8153-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -DTEST_SMP_MEMORY -DRTL8153_HOST_TEST -Isrc/include -Isrc/drivers -Isrc/kernel -Isrc/arch/x86_64 tests/rtl8153_host.c src/drivers/rtl8153.c -o $(BUILD_DIR)/rtl8153_host
	@$(BUILD_DIR)/rtl8153_host

test-hub-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -DTEST_SMP_MEMORY -Isrc/include -Isrc/drivers -Isrc/kernel -Isrc/arch/x86_64 -Isrc/fs tests/hub_host.c src/drivers/xhci_rings.c src/drivers/keyboard.c -o $(BUILD_DIR)/hub_host
	@$(BUILD_DIR)/hub_host

test-hub-host-tsan:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=thread -Wall -Wextra -Werror -no-pie -pthread -DTEST_SMP_MEMORY -Isrc/include -Isrc/drivers -Isrc/kernel -Isrc/arch/x86_64 -Isrc/fs tests/hub_host.c src/drivers/xhci_rings.c src/drivers/keyboard.c -o $(BUILD_DIR)/hub_host_tsan
	@timeout 120 setarch x86_64 -R env TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" $(BUILD_DIR)/hub_host_tsan

test-poll-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/kernel tests/poll_host.c -o $(BUILD_DIR)/poll_host
	@$(BUILD_DIR)/poll_host

test-poll-nc: $(BOOTABLE_ISO)
	@python3 scripts/test_poll_nc.py

test-usb-hid: $(BOOTABLE_ISO)
	@python3 scripts/test_usb_hid.py

test-usb-hub: $(BOOTABLE_ISO)
	@python3 scripts/test_usb_hub.py

test-usb-mouse: $(BOOTABLE_ISO)
	@python3 scripts/test_usb_mouse.py




USER_DIR := user
USER_INIT_ELF := $(BUILD_DIR)/init.elf
USER_HELLO_ELF := $(BUILD_DIR)/hello.elf
USER_DUAL_STREAM_ELF := $(BUILD_DIR)/dual_stream.elf
USER_SHELL_ELF := $(BUILD_DIR)/shell.elf
USER_SH_BUILTIN_ELF := $(BUILD_DIR)/sh-builtin.elf
USER_PS_ELF := $(BUILD_DIR)/ps.elf
USER_SYSINFO_ELF := $(BUILD_DIR)/sysinfo.elf
USER_DMESG_ELF := $(BUILD_DIR)/dmesg.elf
USER_IFCONFIG_ELF := $(BUILD_DIR)/ifconfig.elf
USER_IFUP_ELF := $(BUILD_DIR)/ifup.elf
USER_WIFI_ELF := $(BUILD_DIR)/wifi.elf
USER_PING_ELF := $(BUILD_DIR)/ping.elf
USER_PING_PROBE_ELF := $(BUILD_DIR)/net_ping_probe.elf
USER_UDP_ELFS := $(BUILD_DIR)/udptest.elf $(BUILD_DIR)/net_udp_probe.elf
USER_TCP_ELF := $(BUILD_DIR)/tcptest.elf
USER_TCP_SERVER_ELF := $(BUILD_DIR)/tcpserve.elf
USER_NC_ELF := $(BUILD_DIR)/nc.elf
USER_NSLOOKUP_ELF := $(BUILD_DIR)/nslookup.elf
USER_DNSPROBE_ELF := $(BUILD_DIR)/dnsprobe.elf
USER_TCPDEADLINE_ELF := $(BUILD_DIR)/tcpdeadline.elf
USER_WGET_ELF := $(BUILD_DIR)/wget.elf
USER_DOWNLOAD_ELF := $(BUILD_DIR)/download.elf
USER_DHCP_PROBE_ELF := $(BUILD_DIR)/dhcp-probe.elf
USER_DHCPC_ELF := $(BUILD_DIR)/dhcpc.elf
USER_WATCHTOWER_ELF := $(BUILD_DIR)/watchtower.elf
USER_HTTPD_ELF := $(BUILD_DIR)/httpd.elf
USER_PTHREAD_TEST_ELF := $(BUILD_DIR)/pthread-test.elf
USER_CRASH_TEST_ELF := $(BUILD_DIR)/crash-test.elf
USER_TELNET_ELF := $(BUILD_DIR)/telnet.elf
USER_SSH_ELF := $(BUILD_DIR)/ssh.elf
USER_SERVICE_ELF := $(BUILD_DIR)/service.elf
USER_CROND_ELF := $(BUILD_DIR)/crond.elf
USER_LOGROTATE_ELF := $(BUILD_DIR)/logrotate.elf
DNS_OBJECTS := $(BUILD_DIR)/dns.o $(BUILD_DIR)/dns_codec.o
$(DNS_OBJECTS): $(BUILD_DIR)/%.o: user/%.c user/dns.h user/dns_codec.h src/include/syscall_abi.h src/include/socket_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(BUILD_DIR)/netconf.o: user/netconf.c user/netconf.h src/include/types.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(BUILD_DIR)/nslookup.o: user/nslookup.c user/dns.h user/dns_codec.h user/udp_common.h user/netconf.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -c $< -o $@
$(USER_NSLOOKUP_ELF): $(BUILD_DIR)/nslookup.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=nslookup_main user/tools/start.asm -o $(BUILD_DIR)/nslookup_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/nslookup_start.o $(BUILD_DIR)/nslookup.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o -o $@
$(BUILD_DIR)/dnsprobe.o: user/dnsprobe.c user/dns.h user/udp_common.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -c $< -o $@
$(USER_DNSPROBE_ELF): $(BUILD_DIR)/dnsprobe.o $(DNS_OBJECTS) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=dnsprobe_main user/tools/start.asm -o $(BUILD_DIR)/dnsprobe_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/dnsprobe_start.o $(BUILD_DIR)/dnsprobe.o $(DNS_OBJECTS) -o $@
$(BUILD_DIR)/tcpdeadline.o: user/tcpdeadline.c user/udp_common.h src/include/syscall_abi.h src/include/socket_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -c $< -o $@
$(USER_TCPDEADLINE_ELF): $(BUILD_DIR)/tcpdeadline.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=tcpdeadline_main user/tools/start.asm -o $(BUILD_DIR)/tcpdeadline_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/tcpdeadline_start.o $(BUILD_DIR)/tcpdeadline.o -o $@
$(BUILD_DIR)/nc.o: user/nc.c user/dns.h user/udp_common.h user/netconf.h src/include/socket_abi.h src/include/syscall_abi.h src/include/terminal.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $@
$(USER_NC_ELF): $(BUILD_DIR)/nc.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=nc_main user/tools/start.asm -o $(BUILD_DIR)/nc_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/nc_start.o $(BUILD_DIR)/nc.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o -o $@
$(BUILD_DIR)/wget_codec.o: user/wget_codec.c user/wget_codec.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(BUILD_DIR)/wget.o: user/wget.c user/wget_codec.h user/dns.h user/dns_codec.h user/udp_common.h user/netconf.h src/include/socket_abi.h src/include/syscall_abi.h src/fs/vfs.h src/include/tls.h $(TLS_LIB)
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Isrc/lib/tls/bearssl/inc -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_WGET_ELF): $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(TLS_LIB) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=wget_main user/tools/start.asm -o $(BUILD_DIR)/wget_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/wget_start.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(TLS_LIB) -o $@
	@strip --strip-debug $@
$(BUILD_DIR)/download.o: user/download.c user/wget.h user/udp_common.h src/include/syscall_abi.h src/include/socket_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_DOWNLOAD_ELF): $(BUILD_DIR)/download.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(TLS_LIB) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=download_main user/tools/start.asm -o $(BUILD_DIR)/download_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/download_start.o $(BUILD_DIR)/download.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(TLS_LIB) -o $@
	@strip --strip-debug $@
$(BUILD_DIR)/httpd.o: user/httpd.c user/poll.h user/udp_common.h src/include/socket_abi.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_HTTPD_ELF): $(BUILD_DIR)/httpd.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=httpd_main user/tools/start.asm -o $(BUILD_DIR)/httpd_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/httpd_start.o $(BUILD_DIR)/httpd.o -o $@
	@strip --strip-debug $@

$(BUILD_DIR)/pthread_test.o: user/pthread_test.c user/pthread.h user/tools/common.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -I. -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_PTHREAD_TEST_ELF): $(BUILD_DIR)/pthread_test.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=pthread_test_main user/tools/start.asm -o $(BUILD_DIR)/pthread_test_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/pthread_test_start.o $(BUILD_DIR)/pthread_test.o $(BUILD_DIR)/tool-common.o -o $@
	@strip --strip-debug $@

$(BUILD_DIR)/crash_test.o: user/crash_test.c user/tools/common.h src/include/syscall_abi.h src/include/signal_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -I. -Isrc/include -Iuser/tools -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_CRASH_TEST_ELF): $(BUILD_DIR)/crash_test.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=crash_test_main user/tools/start.asm -o $(BUILD_DIR)/crash_test_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/crash_test_start.o $(BUILD_DIR)/crash_test.o $(BUILD_DIR)/tool-common.o -o $@
	@strip --strip-debug $@

$(BUILD_DIR)/telnet.o: user/telnet.c user/poll.h user/udp_common.h user/dns.h user/netconf.h src/include/socket_abi.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_TELNET_ELF): $(BUILD_DIR)/telnet.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=telnet_main user/tools/start.asm -o $(BUILD_DIR)/telnet_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/telnet_start.o $(BUILD_DIR)/telnet.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o -o $@
	@strip --strip-debug $@

$(BUILD_DIR)/ssh.o: user/ssh.c user/poll.h user/udp_common.h user/dns.h user/netconf.h user/sshd/sshbuf.h user/sshd/cipher.h user/sshd/ssh_packet.h src/include/crypto.h src/include/socket_abi.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Iuser/sshd -Iuser/tools -Isrc/fs -Os -fno-pie -fno-asynchronous-unwind-tables -Wframe-larger-than=1024 -fstack-usage -c $< -o $@

$(USER_SSH_ELF): $(BUILD_DIR)/ssh.o $(BUILD_DIR)/sshd/sshbuf.o $(BUILD_DIR)/sshd/cipher.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(BUILD_DIR)/tool-common.o $(CRYPTO_LIB) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=ssh_main user/tools/start.asm -o $(BUILD_DIR)/ssh_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 --gc-sections -T user/shell.ld $(BUILD_DIR)/ssh_start.o $(BUILD_DIR)/ssh.o $(BUILD_DIR)/sshd/sshbuf.o $(BUILD_DIR)/sshd/cipher.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o $(BUILD_DIR)/tool-common.o $(CRYPTO_LIB) -o $@
	@strip --strip-debug $@

$(BUILD_DIR)/dhcp_codec.o: user/dhcp_codec.c user/dhcp_codec.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(BUILD_DIR)/dhcp_probe.o: user/tools/dhcp-probe.c user/dhcp_codec.h src/include/types.h src/include/syscall_abi.h src/include/socket_abi.h src/include/netctl_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_DHCP_PROBE_ELF): $(BUILD_DIR)/dhcp_probe.o $(BUILD_DIR)/dhcp_codec.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=dhcp_probe_main user/tools/start.asm -o $(BUILD_DIR)/dhcp_probe_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/dhcp_probe_start.o $(BUILD_DIR)/dhcp_probe.o $(BUILD_DIR)/dhcp_codec.o -o $@
$(BUILD_DIR)/timeline_log.o: user/timeline_log.c user/timeline_log.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Iuser -Iuser/tools -c $< -o $@

$(BUILD_DIR)/dhcpc.o: user/dhcpc.c user/timeline_log.h user/dhcp_codec.h user/resolv_conf.h user/machine_config.h user/netconf.h src/include/types.h src/include/syscall_abi.h src/include/socket_abi.h src/include/netctl_abi.h src/include/account_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Iuser -Iuser/tools -c $< -o $@
$(USER_DHCPC_ELF): $(BUILD_DIR)/timezone.o $(BUILD_DIR)/timezone_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/dhcpc.o $(BUILD_DIR)/dhcp_codec.o $(BUILD_DIR)/timeline_log.o $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=dhcpc_main user/tools/start.asm -o $(BUILD_DIR)/dhcpc_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/dhcpc_start.o $(BUILD_DIR)/dhcpc.o $(BUILD_DIR)/dhcp_codec.o $(BUILD_DIR)/timeline_log.o $(BUILD_DIR)/timezone.o $(BUILD_DIR)/timezone_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/netconf.o -o $@
# Ring 3 has a one-page user stack. Bound individual frames as well as auditing
# the nested call chain; host sanitizer stacks cannot establish this property.
$(BUILD_DIR)/watchtower_service.o $(BUILD_DIR)/watchtower_config.o $(BUILD_DIR)/watchtower_ctl.o $(BUILD_DIR)/watchtower_main.o $(BUILD_DIR)/service_cli.o: CFLAGS += -Wframe-larger-than=1024

$(BUILD_DIR)/watchtower_service.o: user/watchtower/watchtower_service.c user/watchtower/watchtower.h user/watchtower/watchtower_config.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/watchtower -c $< -o $@
$(BUILD_DIR)/watchtower_config.o: user/watchtower/watchtower_config.c user/watchtower/watchtower_config.h user/watchtower/watchtower.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/watchtower -c $< -o $@
$(BUILD_DIR)/watchtower_ctl.o: user/watchtower/watchtower_ctl.c user/watchtower/watchtower_ctl.h user/watchtower/watchtower.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/watchtower -c $< -o $@
$(BUILD_DIR)/watchtower_main.o: user/watchtower/watchtower_main.c user/watchtower/watchtower.h user/watchtower/watchtower_config.h user/watchtower/watchtower_ctl.h user/watchtower/watchtower_identity.h user/tools/userdb.h src/include/types.h src/include/syscall_abi.h src/include/signal_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/watchtower -c $< -o $@
$(BUILD_DIR)/watchtower_identity.o: user/watchtower/watchtower_identity.c user/watchtower/watchtower_identity.h user/watchtower/watchtower.h user/tools/userdb.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Wframe-larger-than=1024 -Isrc/include -Iuser/watchtower -c $< -o $@
$(USER_WATCHTOWER_ELF): $(BUILD_DIR)/watchtower_service.o $(BUILD_DIR)/watchtower_config.o $(BUILD_DIR)/watchtower_ctl.o $(BUILD_DIR)/watchtower_main.o $(BUILD_DIR)/watchtower_identity.o $(BUILD_DIR)/service_cli.o $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/tool-digest.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=watchtower_main user/tools/start.asm -o $(BUILD_DIR)/watchtower_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/watchtower_start.o $(BUILD_DIR)/watchtower_service.o $(BUILD_DIR)/watchtower_config.o $(BUILD_DIR)/watchtower_ctl.o $(BUILD_DIR)/watchtower_main.o $(BUILD_DIR)/watchtower_identity.o $(BUILD_DIR)/service_cli.o $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/tool-digest.o -o $@
$(BUILD_DIR)/service_cli.o: user/watchtower/service_cli.c user/watchtower/watchtower.h user/watchtower/watchtower_ctl.h src/include/types.h src/include/syscall_abi.h src/include/signal_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/watchtower -c $< -o $@
$(USER_SERVICE_ELF): $(BUILD_DIR)/service_cli.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=service_main user/tools/start.asm -o $(BUILD_DIR)/service_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/service_start.o $< -o $@
$(BUILD_DIR)/crond.o: user/crond/crond.c user/crond/crond.h src/include/types.h src/include/syscall_abi.h src/include/signal_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/crond -c $< -o $@
$(USER_CROND_ELF): $(BUILD_DIR)/crond.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=crond_main user/tools/start.asm -o $(BUILD_DIR)/crond_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/crond_start.o $< -o $@
$(BUILD_DIR)/logrotate.o: user/logrotate/logrotate.c user/logrotate/logrotate.h src/include/types.h src/include/syscall_abi.h src/include/signal_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Iuser/logrotate -c $< -o $@
$(USER_LOGROTATE_ELF): $(BUILD_DIR)/logrotate.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=logrotate_main user/tools/start.asm -o $(BUILD_DIR)/logrotate_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/logrotate_start.o $< -o $@
$(BUILD_DIR)/tcpserve.o: user/tcpserve.c user/udp_common.h src/include/socket_abi.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $@
$(USER_TCP_SERVER_ELF): $(BUILD_DIR)/tcpserve.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=tcpserve_main user/tools/start.asm -o $(BUILD_DIR)/tcpserve_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/tcpserve_start.o $< -o $@
$(BUILD_DIR)/tcptest.o: user/tcptest.c user/udp_common.h src/include/socket_abi.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $@
$(USER_TCP_ELF): $(BUILD_DIR)/tcptest.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=tcptest_main user/tools/start.asm -o $(BUILD_DIR)/tcptest_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/tcptest_start.o $< -o $@

$(BUILD_DIR)/udptest.o $(BUILD_DIR)/net_udp_probe.o: $(BUILD_DIR)/%.o: user/%.c user/udp_common.h src/include/socket_abi.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -Wframe-larger-than=512 -fstack-usage -c $< -o $@
$(USER_UDP_ELFS): $(BUILD_DIR)/%.elf: $(BUILD_DIR)/%.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/$*_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/$*_start.o $< -o $@

$(BUILD_DIR)/net/net_socket.o $(BUILD_DIR)/net/net_socket_syscall.o $(BUILD_DIR)/net/udp.o $(BUILD_DIR)/net/net_ipv4.o: CFLAGS += -Os -Wframe-larger-than=512 -fstack-usage
USER_TOP_ELF := $(BUILD_DIR)/top.elf
USER_NANO_ELF := $(BUILD_DIR)/nano.elf
SBIN_STREAM_TOOLS := shutdown reboot poweroff lockstat
USER_STREAM_TOOLS := cat head tail wc grep uniq xxd sort diff patch sed awk diskbench smpbench chmod chown mkdir rm mv cp touch hostname free which env date sleep time gzip gunzip kill logger base64
$(BUILD_DIR)/tool-gunzip.o: CFLAGS += -DGUNZIP_STANDALONE
STREAM_TOOLS := $(USER_STREAM_TOOLS) $(SBIN_STREAM_TOOLS)
SBIN_LOGIN_TOOLS := useradd addgroup delgroup adduser deluser usermod
USER_LOGIN_TOOLS := login whoami sudo recovery passwd su groups nologin id
LOGIN_TOOLS := $(USER_LOGIN_TOOLS) $(SBIN_LOGIN_TOOLS)
LOGIN_ELFS := $(addprefix $(BUILD_DIR)/tool-,$(addsuffix .elf,$(LOGIN_TOOLS)))

$(BUILD_DIR)/tool-hostname.o $(BUILD_DIR)/netconf.o $(BUILD_DIR)/ifup.o $(BUILD_DIR)/ifconfig.o: user/machine_config.h src/include/account_abi.h

ifeq ($(LOGIN_TEST),1)
CFLAGS += -DTEST_LOGIN_BOOT
endif
ifeq ($(INSTALLER_TEST),1)
CFLAGS += -DTEST_INSTALLER
endif
ifeq ($(INSTALLER_INSPECT),1)
CFLAGS += -DTEST_INSTALLER_INSPECT
endif
ifeq ($(INSTALLER_PHYSICAL),1)
ifneq ($(INSTALLER_TEST)$(INSTALLER_INSPECT),)
$(error Physical installer cannot be combined with disposable or inspection profiles)
endif
CFLAGS += -DINSTALLER_PHYSICAL
endif

ifeq ($(INSTALLER_MULTI),1)
ifneq ($(INSTALLER_TEST)$(INSTALLER_INSPECT)$(INSTALLER_PHYSICAL),)
$(error Multi-drive installer requires a separate build)
endif
CFLAGS += -DINSTALLER_MULTI
endif

$(BUILD_DIR)/tool-userdb.o: user/tools/userdb.c user/tools/userdb.h user/tools/digest.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/tool-userdb_io.o: user/tools/userdb_io.c user/tools/live_password.h user/tools/userdb.h user/tools/common.h user/permissions_cli.h src/include/account_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $@

$(LOGIN_ELFS): $(BUILD_DIR)/tool-%.elf: user/tools/%.c user/tools/userdb.h user/tools/live_password.h user/tools/account_tool.h src/include/account_abi.h user/entry_security.h user/permissions_cli.h $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/tool-digest.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $(BUILD_DIR)/tool-$*.o
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-$*.o $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o -o $@
	@strip --strip-debug $@

.PHONY: stage-user-database test-perm-db-host test-login
stage-user-database:
	@python3 scripts/stage_user_database.py $(BUILD_DIR)/initramfs/etc

test-perm-db-host:
	@python3 scripts/test_user_database_host.py

.PHONY: test-usermgmt-host test-usermgmt-qemu
test-usermgmt-host:
	@gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isrc/include -Iuser/tools tests/usermgmt_host.c user/tools/userdb.c user/tools/digest.c -o $(BUILD_DIR)/test-usermgmt-host
	@$(BUILD_DIR)/test-usermgmt-host

test-usermgmt-qemu: $(BOOTABLE_ISO)
	@python3 scripts/test_usermgmt.py

test-login:
	@python3 scripts/test_login.py

$(BUILD_DIR)/perm_phase3_user.elf: tests/perm_phase3_user.c user/tools/common.h user/permissions_cli.h $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $(BUILD_DIR)/perm_phase3_user.o
	@$(AS) -f elf64 -DTOOL_ENTRY=phase3_main user/tools/start.asm -o $(BUILD_DIR)/perm_phase3_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/perm_phase3_user_start.o $(BUILD_DIR)/perm_phase3_user.o $(BUILD_DIR)/tool-common.o -o $@
STREAM_TOOL_ELFS := $(addprefix $(BUILD_DIR)/tool-,$(addsuffix .elf,$(STREAM_TOOLS)))
USER_DISK_ELF := $(BUILD_DIR)/tool-disk.elf
CHECKSUM_TOOLS := md5sum sha256sum
CHECKSUM_ELFS := $(addprefix $(BUILD_DIR)/tool-,$(addsuffix .elf,$(CHECKSUM_TOOLS)))
USER_TAR_ELF := $(BUILD_DIR)/tool-tar.elf
USER_TRACEROUTE_ELF := $(BUILD_DIR)/traceroute.elf
INITRAMFS_TAR := $(BIN_DIR)/initramfs.tar
include scripts/fstat.mk
include scripts/replace.mk
include scripts/vfs_flags.mk
include scripts/hostkey_acceptance.mk
ifeq ($(CRYPTO_TEST_PROBE),1)
$(INITRAMFS_TAR): $(BUILD_DIR)/crypto-probe.elf
endif

$(BUILD_DIR)/getrandom-probe.elf: user/tools/getrandom-probe.c user/tools/common.h src/include/syscall_abi.h $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $(BUILD_DIR)/getrandom-probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=getrandom_probe_main user/tools/start.asm -o $(BUILD_DIR)/getrandom-probe-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/getrandom-probe-start.o $(BUILD_DIR)/getrandom-probe.o $(BUILD_DIR)/tool-common.o -o $@

$(INITRAMFS_TAR): $(BUILD_DIR)/getrandom-probe.elf

.PHONY: test-random-host test-getrandom
test-random-host:
	@mkdir -p build
	@gcc -std=c11 -Wall -Wextra -Werror -O1 -g -no-pie -DTEST_SMP_MEMORY -DRANDOM_HOST_TEST -pthread -fsanitize=address,undefined -Isrc/include -Isrc/mm -Isrc/lib tests/random_host.c src/lib/hmac_drbg.c src/lib/root_digest.c -o build/random_host
	@build/random_host
	@python3 scripts/test_random_reference.py

test-getrandom: $(BOOTABLE_ISO)
	@python3 scripts/test_getrandom.py
$(INITRAMFS_TAR): $(LOGIN_ELFS) stage-user-database scripts/stage_user_database.py src/lib/crypto/LICENCE.md
$(INITRAMFS_TAR): $(BUILD_DIR)/ssh-keygen.elf

$(BUILD_DIR)/tool-common.o: user/tools/common.c user/tools/common.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/shell-jobctl.o $(BUILD_DIR)/tool-kill.o: user/kill_cli.h
$(BUILD_DIR)/tool-logger.o: user/tools/logger.h
$(BUILD_DIR)/tool-boot_snapshot.o: user/tools/boot_snapshot.h

USER_BOOT_SNAPSHOT_ELF := $(BUILD_DIR)/boot-snapshot.elf
$(BUILD_DIR)/tool-boot_snapshot.o: user/tools/boot_snapshot.c user/tools/boot_snapshot.h user/tools/common.h user/permissions_cli.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Isrc/include -Isrc/fs -Iuser/tools -c $< -o $@

$(USER_BOOT_SNAPSHOT_ELF): $(BUILD_DIR)/tool-boot_snapshot.o user/tools/start.asm user/shell.ld $(BUILD_DIR)/tool-common.o
	@$(AS) -f elf64 -DTOOL_ENTRY=boot_snapshot_main user/tools/start.asm -o $(BUILD_DIR)/tool-boot_snapshot-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-boot_snapshot-start.o $(BUILD_DIR)/tool-boot_snapshot.o $(BUILD_DIR)/tool-common.o -o $@

$(addprefix $(BUILD_DIR)/tool-,$(addsuffix .o,$(STREAM_TOOLS))): $(BUILD_DIR)/tool-%.o: user/tools/%.c user/tools/common.h user/permissions_cli.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(STREAM_TOOL_ELFS): $(BUILD_DIR)/tool-%.elf: $(BUILD_DIR)/tool-%.o user/tools/start.asm user/shell.ld $(BUILD_DIR)/tool-common.o
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-$*.o $(BUILD_DIR)/tool-common.o $(TIME_EXTRA) -o $@
	@strip --strip-debug $@

$(USER_DISK_ELF): user/tools/disk.c $(BUILD_DIR)/tool-diskbench.elf $(BUILD_DIR)/tool-diskbench.o user/tools/common.h user/tools/start.asm user/shell.ld $(BUILD_DIR)/tool-common.o src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $(BUILD_DIR)/tool-disk.o
	@$(AS) -f elf64 -DTOOL_ENTRY=disk_main user/tools/start.asm -o $(BUILD_DIR)/tool-disk-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-disk-start.o $(BUILD_DIR)/tool-disk.o $(BUILD_DIR)/tool-diskbench.o $(BUILD_DIR)/tool-common.o -o $@

.PHONY: test-stream-tools-host
$(BUILD_DIR)/tool-digest.o: user/tools/digest.c user/tools/digest.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/tool-checksum.o: user/tools/checksum.c user/tools/digest.h user/tools/common.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(CHECKSUM_ELFS): $(BUILD_DIR)/tool-%.elf: $(BUILD_DIR)/tool-checksum.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-checksum.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o -o $@

$(BUILD_DIR)/tool-tar.o: user/tools/tar.c user/tools/common.h user/tools/digest.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_TAR_ELF): $(BUILD_DIR)/tool-tar.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=tar_main user/tools/start.asm -o $(BUILD_DIR)/tool-tar-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-tar-start.o $(BUILD_DIR)/tool-tar.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o -o $@

.PHONY: test-tar-host test-tar
test-tar-host:
	@python3 scripts/test_tar_host.py

test-tar: $(BOOTABLE_ISO) nvme-gpt-disk test-tar-host
	@python3 scripts/test_tar.py

.PHONY: test-checksum-host test-checksum
test-checksum-host:
	@python3 scripts/test_checksum_host.py

test-checksum: $(BOOTABLE_ISO) test-checksum-host
	@python3 scripts/test_checksum.py

test-stream-tools-host:
	@python3 scripts/test_stream_tools_host.py

.PHONY: test-base64-host test-base64
test-base64-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/base64_host.c user/tools/common.c user/tools/base64.c -o $(BUILD_DIR)/base64_host && $(BUILD_DIR)/base64_host

test-base64: $(BOOTABLE_ISO) test-base64-host
	@python3 scripts/test_base64_qemu.py

.PHONY: test-httpd-host test-httpd
test-httpd-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTEST_HOST -Isrc/include -Isrc/fs -Iuser tests/httpd_host.c -o $(BUILD_DIR)/httpd_host && $(BUILD_DIR)/httpd_host

test-httpd: $(BOOTABLE_ISO) test-httpd-host
	@python3 scripts/test_httpd.py

.PHONY: test-telnet-host test-telnet
test-telnet-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTEST_HOST -Isrc/include -Isrc/fs -Iuser tests/telnet_host.c user/dns.c user/dns_codec.c user/netconf.c -o $(BUILD_DIR)/telnet_host && $(BUILD_DIR)/telnet_host

test-telnet: $(BOOTABLE_ISO) test-telnet-host
	@python3 scripts/test_telnet.py

.PHONY: test-sed-host test-sed
test-sed-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/sed_host.c user/tools/common.c user/tools/sed.c -o $(BUILD_DIR)/sed_host && $(BUILD_DIR)/sed_host

test-sed: $(BOOTABLE_ISO) test-sed-host
	@python3 scripts/test_sed.py

.PHONY: test-awk-host test-awk
test-awk-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/awk_host.c user/tools/common.c user/tools/awk.c -o $(BUILD_DIR)/awk_host && $(BUILD_DIR)/awk_host

test-awk: $(BOOTABLE_ISO) test-awk-host
	@python3 scripts/test_awk.py

.PHONY: test-kill-host
.PHONY: test-pkg-format-host
.PHONY: test-pkg-plan-host
test-pkg-plan-host: scripts/test_pkg_plan.py tests/pkg_plan_candidate.c tests/pkg_plan_candidate.h tests/pkg_plan_host.c
	@python3 scripts/test_pkg_plan.py

test-pkg-format-host:
	@mkdir -p $(BUILD_DIR)/pkg-format
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/pkg_format_host.c user/pkg/format.c -o $(BUILD_DIR)/pkg-format/host
	@$(BUILD_DIR)/pkg-format/host
	@python3 scripts/pkg_manifest_vectors.py
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/pkg_manifest_host.c user/pkg/format.c user/pkg/manifest.c -o $(BUILD_DIR)/pkg-format/manifest-host
	@$(BUILD_DIR)/pkg-format/manifest-host
	@gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -msoft-float -fstack-usage -Wframe-larger-than=1024 -c user/pkg/manifest.c -o $(BUILD_DIR)/pkg-format/manifest.o
	@gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -msoft-float -fstack-usage -Wframe-larger-than=1024 -c user/pkg/format.c -o $(BUILD_DIR)/pkg-format/format.o

.PHONY: test-pkg-verify-host
.PHONY: test-pkg-catalog-host
.PHONY: test-pkg-cli-host
.PHONY: test-pkg-account-ext4-host
test-pkg-account-ext4-host: scripts/test_pkg_account_ext4_host.py tests/pkg_account_ext4_host.c
	@python3 scripts/test_pkg_account_ext4_host.py

.PHONY: test-pkg-account-bounds-host
.PHONY: test-pkg-account-replay-model
test-pkg-account-replay-model: scripts/test_pkg_account_replay_model.py tests/pkg_account_replay_model.py
	@python3 scripts/test_pkg_account_replay_model.py

.PHONY: test-pkg-account-namespace-host
.PHONY: test-pkg-namespace-kernel test-pkg-namespace-kernel-regression test-pkg-private-namespace
test-pkg-private-namespace: scripts/test_pkg_private_namespace.py
	@python3 scripts/test_pkg_private_namespace.py

test-pkg-namespace-kernel: scripts/test_pkg_namespace_kernel.py tests/pkg_namespace_kernel_host.inc src/fs/ext4_account_namespace.inc
	@test -n "$(PKG_NAMESPACE_CAMPAIGN)"
	@python3 scripts/test_pkg_namespace_kernel.py "$(PKG_NAMESPACE_CAMPAIGN)"

test-pkg-namespace-kernel-regression: scripts/test_pkg_namespace_kernel_regression.py
	@test -n "$(PKG_NAMESPACE_KERNEL_CAMPAIGN)"
	@python3 scripts/test_pkg_namespace_kernel_regression.py "$(PKG_NAMESPACE_KERNEL_CAMPAIGN)"

.PHONY: test-pkg-namespace-admission test-pkg-namespace-admission-ext4
test-pkg-namespace-admission: scripts/test_pkg_namespace_admission.py tests/pkg_namespace_admission.py tests/pkg_account_replay_model.py
	@python3 scripts/test_pkg_namespace_admission.py

test-pkg-namespace-admission-ext4: scripts/test_pkg_namespace_admission_ext4.py tests/pkg_namespace_admission.py
	@test -n "$(PKG_NAMESPACE_CAMPAIGN)"
	@python3 scripts/test_pkg_namespace_admission_ext4.py "$(PKG_NAMESPACE_CAMPAIGN)"

test-pkg-account-namespace-host: scripts/test_pkg_account_namespace.py tests/pkg_account_namespace_host.c
	@python3 scripts/test_pkg_account_namespace.py

test-pkg-account-bounds-host: scripts/test_pkg_account_bounds.py
	@test -n "$(PKG_ACCOUNT_CAMPAIGN)"
	@python3 scripts/test_pkg_account_bounds.py "$(PKG_ACCOUNT_CAMPAIGN)"

test-pkg-cli-host: test-pkg-catalog-host
	@python3 scripts/test_pkg_cli_host.py

# Private copied-workspace BIOS/UEFI x SMP=1/4; no data disk or release integration.
.PHONY: test-pkg-cli-qemu
test-pkg-cli-qemu:
	@python3 scripts/test_pkg_cli_qemu.py

.PHONY: test-pkg-stack-qemu
test-pkg-stack-qemu:
	@python3 scripts/test_pkg_stack_qemu.py

test-pkg-catalog-host: test-pkg-verify-host
	@python3 scripts/test_pkg_catalog_host.py

test-pkg-verify-host: test-pkg-format-host
	@python3 scripts/pkg_artifact_vectors.py
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Isrc/include -Isrc/fs -Isrc/kernel -Iuser/tools tests/pkg_verify_host.c user/pkg/format.c user/pkg/manifest.c user/pkg/verify.c user/tools/digest.c user/tools/gzip.c -o $(BUILD_DIR)/pkg-format/verify-host
	@timeout 60s $(BUILD_DIR)/pkg-format/verify-host
	@gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -msoft-float -fstack-usage -Wframe-larger-than=1024 -Isrc/include -Isrc/fs -Isrc/kernel -c user/pkg/verify.c -o $(BUILD_DIR)/pkg-format/verify.o
	@gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -msoft-float -fstack-usage -Wframe-larger-than=1024 -Isrc/include -Isrc/fs -Iuser/tools -c user/tools/gzip.c -o $(BUILD_DIR)/pkg-format/gzip.o
	@gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -msoft-float -fstack-usage -Wframe-larger-than=1024 -Isrc/include -c user/tools/digest.c -o $(BUILD_DIR)/pkg-format/digest.o

test-kill-host:
	@python3 scripts/test_s8_jobs_host.py
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -I. tests/kill_host.c user/tools/kill.c user/tools/common.c -o $(BUILD_DIR)/kill-host
	@$(BUILD_DIR)/kill-host

.PHONY: test-gzip-host
test-gzip-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -Wframe-larger-than=1024 -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/gzip_host.c user/tools/common.c user/tools/digest.c user/tools/gzip.c user/tools/gunzip.c -o $(BUILD_DIR)/gzip_host && $(BUILD_DIR)/gzip_host

.PHONY: test-logger-host
test-logger-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -DPERMISSIONS_CLI_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools -Iuser tests/logger_host.c user/tools/common.c user/tools/logger.c user/timezone.c -o $(BUILD_DIR)/logger_host && $(BUILD_DIR)/logger_host

.PHONY: test-timeline-log-host
test-timeline-log-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTIMELINE_LOG_HOST_TEST -Isrc/include -Isrc/fs -Iuser tests/timeline_log_host.c user/timeline_log.c -o $(BUILD_DIR)/timeline_log_host && $(BUILD_DIR)/timeline_log_host

.PHONY: test-boot-snapshot-host
test-boot-snapshot-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -DPERMISSIONS_CLI_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools -Iuser tests/boot_snapshot_host.c user/tools/common.c user/tools/boot_snapshot.c -o $(BUILD_DIR)/boot_snapshot_host && $(BUILD_DIR)/boot_snapshot_host


.PHONY: test-free-host test-free
test-free-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/free_host.c user/tools/common.c user/tools/free.c -o $(BUILD_DIR)/free_host && $(BUILD_DIR)/free_host

test-free: $(BOOTABLE_ISO) test-free-host
	@python3 scripts/test_free.py

.PHONY: test-time-host test-date-sleep-host test-date-host test-time-cmd-host test-time test-time-cmd
test-time-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DHOST_TEST -Isrc/include -Isrc/drivers src/drivers/rtc.c tests/time_host.c -o $(BUILD_DIR)/time_host && $(BUILD_DIR)/time_host

test-date-sleep-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/date_sleep_host.c user/tools/common.c user/tools/date.c user/tools/sleep.c user/timezone.c user/tools/timezone_io.c -o $(BUILD_DIR)/date_sleep_host && $(BUILD_DIR)/date_sleep_host

test-date-host:
	@mkdir -p $(BUILD_DIR)
	@test -d $(BUILD_DIR)/zoneinfo || python3 scripts/build_zoneinfo.py $(BUILD_DIR)/zoneinfo
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/date_host.c user/tools/common.c user/tools/date.c user/timezone.c user/tools/timezone_io.c -o $(BUILD_DIR)/date_host && $(BUILD_DIR)/date_host

test-time-cmd-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/time_cmd_host.c user/tools/common.c user/tools/time.c -o $(BUILD_DIR)/time_cmd_host && $(BUILD_DIR)/time_cmd_host

test-time: $(BOOTABLE_ISO) test-time-host test-date-sleep-host test-time-cmd-host
	@python3 scripts/test_time.py

test-time-cmd: $(BOOTABLE_ISO) test-time-cmd-host
	@python3 scripts/test_time_cmd.py

.PHONY: test-tail-f-host test-tail-follow-host test-tail-f
test-tail-follow-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser/tools tests/tail_follow_host.c user/tools/common.c user/tools/tail.c -o $(BUILD_DIR)/tail_follow_host && $(BUILD_DIR)/tail_follow_host

test-tail-f-host: test-tail-follow-host

test-tail-f: $(BOOTABLE_ISO) $(NVME_GPT_IMG) test-tail-f-host
	@python3 scripts/test_tail_f.py

.PHONY: test-dmesg-filter-host test-dmesg-filter test-dmesg
test-dmesg-filter-host:
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer -fno-builtin-puts -Isrc/include -Isrc/fs -Iuser -Iuser/shell tests/dmesg_filter_host.c user/shell/builtin_exec.c user/shell/builtins.c -o $(BUILD_DIR)/dmesg_filter_host && $(BUILD_DIR)/dmesg_filter_host

test-dmesg-filter: $(BOOTABLE_ISO) $(NVME_GPT_IMG) test-dmesg-filter-host
	@python3 scripts/test_dmesg_filter.py

test-dmesg: test-dmesg-filter

$(BUILD_DIR)/traceroute.o: user/traceroute.c user/tools/common.h src/include/trace_abi.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_TRACEROUTE_ELF): $(BUILD_DIR)/traceroute.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=traceroute_main user/tools/start.asm -o $(BUILD_DIR)/traceroute_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/traceroute_start.o $(BUILD_DIR)/traceroute.o $(BUILD_DIR)/tool-common.o -o $@

$(BUILD_DIR)/trace_probe.o: user/trace_probe.c user/tools/common.h src/include/trace_abi.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $@
$(BUILD_DIR)/trace_probe.elf: $(BUILD_DIR)/trace_probe.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=trace_probe_main user/tools/start.asm -o $(BUILD_DIR)/trace_probe_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/trace_probe_start.o $(BUILD_DIR)/trace_probe.o $(BUILD_DIR)/tool-common.o -o $@

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

$(USER_DUAL_STREAM_ELF): $(USER_DIR)/dual_stream.asm $(USER_DIR)/linker.ld
	@mkdir -p $(BUILD_DIR)
	@echo "  [AS]  $<"
	@$(AS) -f elf64 $< -o $(BUILD_DIR)/dual_stream.o
	@echo "  [LD]  $@"
	@$(LD) -m elf_x86_64 -nostdlib -static -T $(USER_DIR)/linker.ld $(BUILD_DIR)/dual_stream.o -o $@

# Freestanding user shell, separate address-space ELF (no host runtime).
SHELL_MODULES := $(wildcard user/shell/*.c)
SHELL_HEADERS := user/entry_security.h $(wildcard user/shell/*.h) src/include/terminal.h src/include/syscall_abi.h user/tools/userdb.h user/machine_config.h src/include/account_abi.h
SHELL_OBJECTS := $(patsubst user/shell/%.c,$(BUILD_DIR)/shell-%.o,$(SHELL_MODULES)) $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-digest.o

$(BUILD_DIR)/shell-%.o: user/shell/%.c $(SHELL_HEADERS)
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_SHELL_ELF): $(SHELL_OBJECTS) $(SHELL_HEADERS) $(USER_DIR)/shell.c $(USER_DIR)/shell_start.asm $(USER_DIR)/shell.ld src/fs/vfs.h src/include/types.h src/kernel/syscall.h src/arch/x86_64/idt.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $(USER_DIR)/shell.c -o $(BUILD_DIR)/shell.o
	@$(AS) -f elf64 -DSHELL_RETURN_STATUS $(USER_DIR)/shell_start.asm -o $(BUILD_DIR)/shell_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/shell_start.o $(BUILD_DIR)/shell.o $(SHELL_OBJECTS) -o $@

# Runner ELF for /bin/sh-builtin: links builtin_exec, builtins, io (no UI/history/alias/editor).
SH_BUILTIN_OBJECTS := $(BUILD_DIR)/shell-builtin_exec.o $(BUILD_DIR)/shell-builtins.o $(BUILD_DIR)/shell-io.o $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-digest.o
$(USER_SH_BUILTIN_ELF): $(SH_BUILTIN_OBJECTS) $(SHELL_HEADERS) $(USER_DIR)/sh_builtin_main.c $(USER_DIR)/sh_builtin_start.asm $(USER_DIR)/shell.ld src/fs/vfs.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $(USER_DIR)/sh_builtin_main.c -o $(BUILD_DIR)/sh_builtin_main.o
	@$(AS) -f elf64 $(USER_DIR)/sh_builtin_start.asm -o $(BUILD_DIR)/sh_builtin_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/sh_builtin_start.o $(BUILD_DIR)/sh_builtin_main.o $(SH_BUILTIN_OBJECTS) -o $@

$(BUILD_DIR)/ps.o: $(USER_DIR)/ps.c src/include/types.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/ps_start.o: $(USER_DIR)/ps_start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 $< -o $@

$(USER_PS_ELF): $(BUILD_DIR)/ps_start.o $(BUILD_DIR)/ps.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/ps_start.o $(BUILD_DIR)/ps.o -o $@

$(BUILD_DIR)/sysinfo.o: $(USER_DIR)/sysinfo.c src/include/types.h src/include/syscall_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -DBUILD_GIT_HASH=\"$(BUILD_GIT_HASH)\" -DBUILD_DATE=\"$(BUILD_DATE)\" -c $< -o $@

$(BUILD_DIR)/ping.o: $(USER_DIR)/ping.c $(USER_DIR)/dns.h $(USER_DIR)/dns_codec.h src/include/types.h src/include/syscall_abi.h src/include/ping_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -Wframe-larger-than=512 -fstack-usage -I$(USER_DIR) -c $< -o $@

$(BUILD_DIR)/ping_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=ping_main $< -o $@

$(USER_PING_ELF): $(BUILD_DIR)/ping_start.o $(BUILD_DIR)/ping.o $(DNS_OBJECTS) $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/ping_start.o $(BUILD_DIR)/ping.o $(DNS_OBJECTS) -o $@

$(BUILD_DIR)/net_ping_probe.o: $(USER_DIR)/net_ping_probe.c src/include/syscall_abi.h src/include/ping_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -Wframe-larger-than=512 -fstack-usage -c $< -o $@

$(BUILD_DIR)/net_ping_probe_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=net_ping_probe_main $< -o $@

$(USER_PING_PROBE_ELF): $(BUILD_DIR)/net_ping_probe_start.o $(BUILD_DIR)/net_ping_probe.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/net_ping_probe_start.o $(BUILD_DIR)/net_ping_probe.o -o $@

$(BUILD_DIR)/sysinfo_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=sysinfo_main $< -o $@

$(USER_SYSINFO_ELF): $(BUILD_DIR)/sysinfo_start.o $(BUILD_DIR)/sysinfo.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/sysinfo_start.o $(BUILD_DIR)/sysinfo.o -o $@

$(BUILD_DIR)/dmesg.o: user/dmesg.c $(SHELL_HEADERS) src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Iuser -c $< -o $@

$(BUILD_DIR)/dmesg_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=dmesg_main $< -o $@

$(USER_DMESG_ELF): $(BUILD_DIR)/dmesg_start.o $(BUILD_DIR)/dmesg.o $(SH_BUILTIN_OBJECTS) $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/dmesg_start.o $(BUILD_DIR)/dmesg.o $(SH_BUILTIN_OBJECTS) -o $@

$(BUILD_DIR)/ifconfig.o: $(USER_DIR)/ifconfig.c $(USER_DIR)/resolv_conf.h src/include/types.h src/include/syscall_abi.h src/include/netctl_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -I$(USER_DIR) -c $< -o $@

$(BUILD_DIR)/ifconfig_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=ifconfig_main $< -o $@

$(USER_IFCONFIG_ELF): $(BUILD_DIR)/ifconfig_start.o $(BUILD_DIR)/ifconfig.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/ifconfig_start.o $(BUILD_DIR)/ifconfig.o -o $@

$(BUILD_DIR)/ifup.o: $(USER_DIR)/ifup.c $(USER_DIR)/resolv_conf.h $(USER_DIR)/netconf.h src/include/types.h src/include/syscall_abi.h src/include/netctl_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -I$(USER_DIR) -c $< -o $@

$(BUILD_DIR)/ifup_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=ifup_main $< -o $@

$(USER_IFUP_ELF): $(BUILD_DIR)/ifup_start.o $(BUILD_DIR)/ifup.o $(BUILD_DIR)/netconf.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/ifup_start.o $(BUILD_DIR)/ifup.o $(BUILD_DIR)/netconf.o -o $@

$(BUILD_DIR)/wifi.o: $(USER_DIR)/wifi.c src/include/types.h src/include/syscall_abi.h src/include/wifi_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -I$(USER_DIR) -Isrc/include -c $< -o $@

$(BUILD_DIR)/wifi_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=wifi_main $< -o $@

$(USER_WIFI_ELF): $(BUILD_DIR)/wifi_start.o $(BUILD_DIR)/wifi.o $(BUILD_DIR)/tool-gzip.o $(BUILD_DIR)/tool-common.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/wifi_start.o $(BUILD_DIR)/wifi.o $(BUILD_DIR)/tool-gzip.o $(BUILD_DIR)/tool-common.o -o $@

$(BUILD_DIR)/top.o: $(USER_DIR)/top.c src/include/types.h src/include/syscall_abi.h src/include/terminal.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/top_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=top_main $< -o $@

$(USER_TOP_ELF): $(BUILD_DIR)/top_start.o $(BUILD_DIR)/top.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/top_start.o $(BUILD_DIR)/top.o -o $@

$(BUILD_DIR)/nano.o: $(USER_DIR)/nano.c $(USER_DIR)/nano.h $(USER_DIR)/nano_core.c src/include/types.h src/include/syscall_abi.h src/include/terminal.h src/fs/vfs.h src/include/signal_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -I$(USER_DIR) -c $< -o $@

$(BUILD_DIR)/nano_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=nano_main $< -o $@

$(USER_NANO_ELF): $(BUILD_DIR)/nano_start.o $(BUILD_DIR)/nano.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/nano_start.o $(BUILD_DIR)/nano.o -o $@

# Build USTAR Initramfs archive
$(INITRAMFS_TAR): $(USER_INIT_ELF) $(USER_HELLO_ELF) $(USER_DUAL_STREAM_ELF) $(USER_SHELL_ELF) $(USER_SH_BUILTIN_ELF) $(USER_PS_ELF) $(USER_SYSINFO_ELF) $(USER_DMESG_ELF) $(USER_BOOT_SNAPSHOT_ELF) $(USER_IFCONFIG_ELF) $(USER_IFUP_ELF) $(USER_WIFI_ELF) $(USER_TOP_ELF) $(USER_NANO_ELF) $(USER_PING_ELF) $(USER_PING_PROBE_ELF) $(USER_UDP_ELFS) $(USER_TCP_ELF) $(USER_TCP_SERVER_ELF) $(USER_NC_ELF) $(USER_NSLOOKUP_ELF) $(USER_DNSPROBE_ELF) $(USER_TCPDEADLINE_ELF) $(USER_WGET_ELF) $(USER_DOWNLOAD_ELF) $(STREAM_TOOL_ELFS) $(USER_DISK_ELF) $(CHECKSUM_ELFS) $(USER_TRACEROUTE_ELF) $(USER_TAR_ELF) $(USER_DHCP_PROBE_ELF) $(USER_DHCPC_ELF) $(USER_WATCHTOWER_ELF) $(USER_HTTPD_ELF) $(USER_PTHREAD_TEST_ELF) $(USER_CRASH_TEST_ELF) $(USER_TELNET_ELF) $(USER_SSH_ELF) $(USER_SERVICE_ELF) $(USER_CROND_ELF) $(USER_LOGROTATE_ELF) $(BUILD_DIR)/ssh-keygen.elf $(BUILD_DIR)/sshd.elf $(BUILD_DIR)/ntpd.elf assets/tzdata2026c.tar.gz scripts/build_zoneinfo.py scripts/build_trust_anchors.py COMMANDS.md Makefile scripts/create_initramfs.py
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/sbin $(BUILD_DIR)/initramfs/etc $(BUILD_DIR)/initramfs/docs $(BIN_DIR)
	@cp -f $(USER_INIT_ELF) $(BUILD_DIR)/initramfs/bin/init
	@cp -f $(USER_SHELL_ELF) $(BUILD_DIR)/initramfs/bin/shell
	@cp -f $(USER_SHELL_ELF) $(BUILD_DIR)/initramfs/bin/sh
	@$(foreach tool,$(USER_LOGIN_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@$(foreach tool,$(SBIN_LOGIN_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/sbin/$(tool);)
	@cp -f $(USER_SH_BUILTIN_ELF) $(BUILD_DIR)/initramfs/bin/sh-builtin
	@cp -f $(USER_HELLO_ELF) $(BUILD_DIR)/initramfs/bin/hello
	@cp -f $(USER_DUAL_STREAM_ELF) $(BUILD_DIR)/initramfs/bin/dual_stream
	@cp -f $(USER_PS_ELF) $(BUILD_DIR)/initramfs/bin/ps
	@cp -f $(USER_SYSINFO_ELF) $(BUILD_DIR)/initramfs/bin/sysinfo
	@cp -f $(USER_DMESG_ELF) $(BUILD_DIR)/initramfs/bin/dmesg
	@cp -f $(USER_BOOT_SNAPSHOT_ELF) $(BUILD_DIR)/initramfs/bin/boot-snapshot
	@cp -f $(USER_IFCONFIG_ELF) $(BUILD_DIR)/initramfs/sbin/ifconfig
	@cp -f $(USER_IFUP_ELF) $(BUILD_DIR)/initramfs/sbin/ifup
	@cp -f $(USER_WIFI_ELF) $(BUILD_DIR)/initramfs/bin/wifi
	@cp -f $(BUILD_DIR)/ntpd.elf $(BUILD_DIR)/initramfs/sbin/ntpd
	@python3 scripts/build_zoneinfo.py $(BUILD_DIR)/zoneinfo
	@python3 scripts/build_zoneinfo.py --pack $(BUILD_DIR)/zoneinfo $(BUILD_DIR)/initramfs/usr/share/zoneinfo.tzdb
	@cp -f $(BUILD_DIR)/zoneinfo/LICENSE $(BUILD_DIR)/initramfs/docs/tzdata-licence.txt
	@cp -f $(USER_DHCPC_ELF) $(BUILD_DIR)/initramfs/sbin/dhcpc
	@cp -f $(USER_WATCHTOWER_ELF) $(BUILD_DIR)/initramfs/sbin/watchtower
	@cp -f $(USER_SERVICE_ELF) $(BUILD_DIR)/initramfs/bin/service
	@cp -f $(USER_SERVICE_ELF) $(BUILD_DIR)/initramfs/bin/watchtower
	@cp -f $(USER_CROND_ELF) $(BUILD_DIR)/initramfs/sbin/crond
	@cp -f $(USER_LOGROTATE_ELF) $(BUILD_DIR)/initramfs/sbin/logrotate
	@cp -f $(USER_PING_ELF) $(BUILD_DIR)/initramfs/bin/ping
	@cp -f $(USER_PING_PROBE_ELF) $(BUILD_DIR)/initramfs/bin/net-ping-probe
	@cp -f $(BUILD_DIR)/udptest.elf $(BUILD_DIR)/initramfs/bin/udptest
	@cp -f $(BUILD_DIR)/net_udp_probe.elf $(BUILD_DIR)/initramfs/bin/net-udp-probe
	@cp -f $(USER_TCP_ELF) $(BUILD_DIR)/initramfs/bin/tcptest
	@cp -f $(USER_TCP_SERVER_ELF) $(BUILD_DIR)/initramfs/bin/tcpserve
	@cp -f $(USER_NC_ELF) $(BUILD_DIR)/initramfs/bin/nc
	@cp -f $(USER_NSLOOKUP_ELF) $(BUILD_DIR)/initramfs/bin/nslookup
	@cp -f $(USER_DNSPROBE_ELF) $(BUILD_DIR)/initramfs/bin/dnsprobe
	@cp -f $(USER_TCPDEADLINE_ELF) $(BUILD_DIR)/initramfs/bin/tcpdeadline
	@cp -f $(USER_WGET_ELF) $(BUILD_DIR)/initramfs/bin/wget
	@cp -f $(USER_HTTPD_ELF) $(BUILD_DIR)/initramfs/bin/httpd
	@cp -f $(USER_PTHREAD_TEST_ELF) $(BUILD_DIR)/initramfs/bin/pthread-test
	@cp -f $(USER_CRASH_TEST_ELF) $(BUILD_DIR)/initramfs/bin/crash-test
	@cp -f $(USER_TELNET_ELF) $(BUILD_DIR)/initramfs/bin/telnet
	@cp -f $(USER_SSH_ELF) $(BUILD_DIR)/initramfs/bin/ssh
	@cp -f $(USER_DOWNLOAD_ELF) $(BUILD_DIR)/initramfs/bin/download
	@cp -f $(USER_DHCP_PROBE_ELF) $(BUILD_DIR)/initramfs/bin/dhcp-probe
	@cp -f $(USER_TOP_ELF) $(BUILD_DIR)/initramfs/bin/top
	@cp -f $(USER_NANO_ELF) $(BUILD_DIR)/initramfs/bin/nano
	@$(foreach tool,$(USER_STREAM_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@cp -f $(BUILD_DIR)/getrandom-probe.elf $(BUILD_DIR)/initramfs/bin/getrandom-probe
	@cp -f src/lib/crypto/LICENCE.md $(BUILD_DIR)/initramfs/docs/monocypher-licence.txt
	@mkdir -p $(BUILD_DIR)/initramfs/sbin
	@cp -f $(BUILD_DIR)/ssh-keygen.elf $(BUILD_DIR)/initramfs/sbin/ssh-keygen
	@cp -f $(BUILD_DIR)/sshd.elf $(BUILD_DIR)/initramfs/sbin/sshd
	@if [ "$(CRYPTO_TEST_PROBE)" = 1 ]; then cp -f $(BUILD_DIR)/crypto-probe.elf $(BUILD_DIR)/initramfs/bin/crypto-probe; else rm -f $(BUILD_DIR)/initramfs/bin/crypto-probe; fi
	@if [ "$(FSTAT_TEST_PROBE)" = 1 ]; then cp -f $(BUILD_DIR)/fstat-probe.elf $(BUILD_DIR)/initramfs/bin/fstat-probe; else rm -f $(BUILD_DIR)/initramfs/bin/fstat-probe; fi
	@if [ "$(REPLACE_TEST_PROBE)" = 1 ]; then cp -f $(BUILD_DIR)/replace-probe.elf $(BUILD_DIR)/initramfs/bin/replace-probe; else rm -f $(BUILD_DIR)/initramfs/bin/replace-probe; fi
	@if [ "$(HOSTKEY_TEST_PROBE)" = 1 ]; then cp -f $(BUILD_DIR)/hostkey-probe.elf $(BUILD_DIR)/initramfs/bin/hostkey-probe; else rm -f $(BUILD_DIR)/initramfs/bin/hostkey-probe; fi
	@$(foreach tool,$(SBIN_STREAM_TOOLS) disk,cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/sbin/$(tool);)
	@cp -f $(BUILD_DIR)/tool-shutdown.elf $(BUILD_DIR)/initramfs/bin/shutdown
	@cp -f $(BUILD_DIR)/tool-reboot.elf $(BUILD_DIR)/initramfs/bin/reboot
	@cp -f $(BUILD_DIR)/tool-poweroff.elf $(BUILD_DIR)/initramfs/bin/poweroff
	@cp -f $(USER_DISK_ELF) $(BUILD_DIR)/initramfs/bin/df
	@cp -f $(USER_DISK_ELF) $(BUILD_DIR)/initramfs/bin/disk
	@$(foreach tool,$(CHECKSUM_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@cp -f $(USER_TRACEROUTE_ELF) $(BUILD_DIR)/initramfs/bin/traceroute
	@cp -f $(USER_TAR_ELF) $(BUILD_DIR)/initramfs/bin/tar

	@printf "========================================================\n  Welcome to FortressOS (x86_64 SMP) — by Pride1922\n  \"Security through Isolation and Elegance\"\n========================================================\n" > $(BUILD_DIR)/initramfs/etc/motd
	@printf "FortressOS Documentation\nThe Ring 3 shell supports help, ls, view and echo.\nExternal cat preserves bytes; head, tail and wc process streams. Use TOOL --help.\nFull command reference available in /docs/commands.txt\n" > $(BUILD_DIR)/initramfs/docs/readme.txt
	@rm -f $(BUILD_DIR)/initramfs/etc/network.conf
	@mkdir -p $(BUILD_DIR)/initramfs/etc/network
	@printf "# Fortress Network Configuration\naddress 10.0.2.15/24\ngateway 10.0.2.2\ndns 10.0.2.3\n" > $(BUILD_DIR)/initramfs/etc/network/network.conf
	@printf "[wifi]\nenabled=true\nauto_connect=true\nssid=\"FortressSecure\"\npsk=\"SuperSecretKey2026\"\nkey_mgmt=WPA2-PSK\n" > $(BUILD_DIR)/initramfs/etc/network/wifi.conf
	@printf "# /etc/network/wlan.conf - FortressOS Wireless Configuration\n" > $(BUILD_DIR)/initramfs/etc/network/wlan.conf
	@mkdir -p $(BUILD_DIR)/initramfs/etc/ssh
	@printf "Enabled yes\nPort 22\nListenAddress 0.0.0.0\nPermitRootLogin no\nPasswordAuthentication yes\nPubkeyAuthentication yes\nMaxSessions 2\nMaxAuthTries 5\nLoginGraceTime 60\n" > $(BUILD_DIR)/initramfs/etc/ssh/sshd_config
	@mkdir -p $(BUILD_DIR)/initramfs/etc/watchtower
	@printf "Exec=/bin/boot-snapshot\nOrder=01\nType=simple\nRestart=no\n" > $(BUILD_DIR)/initramfs/etc/watchtower/boot-log.service
	@printf "Exec=/bin/wifi auto\nOrder=08\nType=simple\nRestart=no\nTimeoutStop=10\n" > $(BUILD_DIR)/initramfs/etc/watchtower/wifi.service
	@printf "Exec=/sbin/dhcpc -f\nOrder=10\nEnabled=yes\nRestart=on-failure\nRestartLimit=3\nRestartDelay=5\nTimeoutStop=5\n" > $(BUILD_DIR)/initramfs/etc/watchtower/dhcpc.service
	@printf "Exec=/sbin/sshd -D\nOrder=20\nEnabled=yes\nRestart=on-failure\nRestartLimit=3\nRestartDelay=5\nTimeoutStop=10\n" > $(BUILD_DIR)/initramfs/etc/watchtower/sshd.service
	@printf "Exec=/sbin/crond -f\nOrder=30\nType=simple\nRestart=on-failure\nTimeoutStop=2\n" > $(BUILD_DIR)/initramfs/etc/watchtower/crond.service
	@printf "Exec=/sbin/ntpd -f pool.ntp.org\nOrder=15\nEnabled=yes\nRestart=on-failure\nTimeoutStop=2\n" > $(BUILD_DIR)/initramfs/etc/watchtower/ntpd.service
	@printf "# /etc/crontab - FortressOS crond configuration\n# <interval_minutes> <absolute_command_path> [args...]\n60 /sbin/logrotate --skip-if-locked /etc/logrotate.conf\n" > $(BUILD_DIR)/initramfs/etc/crontab
	@printf "/var/log/messages {\n    size 10M\n    rotate 4\n    compress\n    missingok\n}\n" > $(BUILD_DIR)/initramfs/etc/logrotate.conf
	@mkdir -p $(BUILD_DIR)/initramfs/etc/ssl/certs
	@if [ -f /etc/ssl/certs/ca-certificates.crt ]; then \
		python3 scripts/build_trust_anchors.py --input /etc/ssl/certs/ca-certificates.crt --out-bin $(BUILD_DIR)/initramfs/etc/ssl/certs/anchors.v1 --out-pem $(BUILD_DIR)/initramfs/etc/ssl/certs/ca-certificates.crt --out-sha256 assets/ssl/ca-certificates.sha256; \
	elif [ -f build/ssl/certs/anchors.v1 ]; then \
		cp -f build/ssl/certs/anchors.v1 $(BUILD_DIR)/initramfs/etc/ssl/certs/anchors.v1; \
		if [ -f build/ssl/certs/ca-certificates.crt ]; then cp -f build/ssl/certs/ca-certificates.crt $(BUILD_DIR)/initramfs/etc/ssl/certs/ca-certificates.crt; fi; \
	fi
	@rm -rf $(BUILD_DIR)/initramfs/lib/firmware
	@mkdir -p $(BUILD_DIR)/initramfs/lib/firmware
	@python3 scripts/strip_firmware.py assets/firmware/iwlwifi-8265-36.ucode $(BUILD_DIR)/initramfs/lib/firmware/iwlwifi-8265-36.ucode.gz
	@echo "  [TAR] Generating USTAR archive $@"
	@python3 scripts/create_initramfs.py $(BUILD_DIR)/initramfs $(INITRAMFS_TAR)

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

# Restorer stub: generate the NASM include from signal_frame.h first.
$(BUILD_DIR)/arch/x86_64/sigrestorer.o: $(SRC_DIR)/arch/x86_64/sigrestorer.asm \
    $(SRC_DIR)/include/signal_frame.h $(SRC_DIR)/include/syscall_abi.h \
    scripts/gen_signal_frame_asm.py
	@mkdir -p $(BUILD_DIR)/arch/x86_64
	@echo "  [GEN] build/signal_frame_asm.inc"
	@python3 scripts/gen_signal_frame_asm.py
	@echo "  [AS]  $<"
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
	@mkdir -p $(ISO_ROOT)/boot/firmware
	@mkdir -p $(ISO_ROOT)/EFI/BOOT
	@cp -f $(KERNEL_ELF) $(ISO_ROOT)/boot/fortress.elf
	@cp -f $(INITRAMFS_TAR) $(ISO_ROOT)/boot/initramfs.tar
	@cp -f $(INITRAMFS_TAR) $(ISO_ROOT)/initramfs.tar
	@if [ -f "assets/firmware/iwlwifi-8265-36.ucode" ]; then cp -f assets/firmware/iwlwifi-8265-36.ucode $(ISO_ROOT)/boot/firmware/; fi
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

# Package bootable raw disk image (dual-boot GPT/ESP + journaled EXT4)
.PHONY: test-default-ext4-image
test-default-ext4-image: $(BOOTABLE_IMG)
	@python3 scripts/test_default_ext4_image.py --firmware bios --cpus 8 --output $(BUILD_DIR)/default-ext4-bios-$(shell date +%s)
	@python3 scripts/test_default_ext4_image.py --firmware uefi --cpus 8 --output $(BUILD_DIR)/default-ext4-uefi-$(shell date +%s)

.PHONY: test-combined-installer test-combined-image
test-combined-installer: $(BOOTABLE_IMG)
	@python3 scripts/test_combined_installer.py

test-combined-image: test-default-ext4-image test-combined-installer

img: $(BOOTABLE_IMG)

$(BOOTABLE_IMG): $(KERNEL_ELF) $(INITRAMFS_TAR) build/tool-install-multi.elf limine.conf scripts/create_boot_img.py scripts/create_combined_media.py scripts/create_installer_media.py scripts/create_installer_payload.py scripts/installer_log_volume.py scripts/initialize_ext4_journal.py limine-setup $(BOOTABLE_ISO)
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

TEST_TSAN_BIN := $(BIN_DIR)/pmm_smp_tsan
TEST_TSAN_CFLAGS := -std=c11 -Wall -Wextra -Werror -g -no-pie -Isrc/include -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/kernel -Isrc/lib -Isrc/fs -DTEST_SMP_MEMORY -pthread -fsanitize=thread

$(TEST_TSAN_BIN): $(TEST_HOST_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_TSAN_CFLAGS) -o $@ $^

.PHONY: test-smp-memory-tsan

test-smp-memory-tsan: $(TEST_TSAN_BIN)
	@echo "--- Running PMM SMP memory ThreadSanitizer test ---"
	@setarch x86_64 -R $(TEST_TSAN_BIN)

# Freestanding QEMU SMP memory stress test across BIOS/UEFI and 1/4/8 CPUs
.PHONY: test-smp-memory

test-smp-memory: bin/fortress.elf bin/initramfs.tar
	@echo "--- Running freestanding QEMU SMP memory stress test ---"
	python3 scripts/test_smp_memory.py

.PHONY: test-shell-s5

test-shell-s5: bin/fortress.iso nvme-gpt-disk
	@python3 scripts/test_shell_s5.py

.PHONY: test-shell-s6

test-shell-s6: bin/fortress.iso nvme-gpt-disk
	@python3 scripts/test_shell_s6.py

$(BUILD_DIR)/pipeline_fixture.elf: tests/pipeline_fixture.c tests/pipeline_fixture_start.asm user/shell.ld src/include/syscall_abi.h src/include/types.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/pipeline_fixture.c -o $(BUILD_DIR)/pipeline_fixture.o
	@$(AS) -f elf64 tests/pipeline_fixture_start.asm -o $(BUILD_DIR)/pipeline_fixture_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/pipeline_fixture_start.o $(BUILD_DIR)/pipeline_fixture.o -o $@

.PHONY: test-shell-s7
test-shell-s7: $(BOOTABLE_ISO) $(NVME_GPT_IMG) $(BUILD_DIR)/pipeline_fixture.elf
	@python3 scripts/test_shell_s7.py

# Test-only Ring 3 exerciser: the runner adds it to a disposable ISO, never the
# production initramfs. Uses the same freestanding ABI as the shell.
$(BUILD_DIR)/s6_resources.elf: tests/s6_resources_user.c tests/s6_resources_start.asm user/shell.ld $(SHELL_HEADERS) src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/s6_resources_user.c -o $(BUILD_DIR)/s6_resources.o
	@$(AS) -f elf64 tests/s6_resources_start.asm -o $(BUILD_DIR)/s6_resources_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s6_resources_start.o $(BUILD_DIR)/s6_resources.o -o $@

.PHONY: test-shell-s6-resources
test-shell-s6-resources: $(BOOTABLE_ISO) $(NVME_GPT_IMG) $(BUILD_DIR)/s6_resources.elf
	@python3 scripts/test_shell_s6_resources.py

.PHONY: test-s8-jobctl-host test-s8-orphans-host
test-s8-jobctl-host: test-s8-jobs-host test-s8-orphans-host

test-s8-orphans-host:
	@python3 scripts/test_process_table_host.py --orphans

$(BUILD_DIR)/s8_jobctl_user.elf: tests/s8_jobctl_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/include/signal_abi.h src/include/terminal.h src/include/types.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c tests/s8_jobctl_user.c -o $(BUILD_DIR)/s8_jobctl_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/s8_jobctl_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/s8_jobctl_user_start.o $(BUILD_DIR)/s8_jobctl_user.o -o $@

.PHONY: test-s8-jobctl
test-s8-jobctl: $(BOOTABLE_ISO) $(BUILD_DIR)/s8_jobctl_user.elf $(BUILD_DIR)/s8_jobs_delay_user.elf
	@SMP=$(SMP) python3 scripts/test_s8_jobctl.py

# Networking Phase 5: codecs plus actual stack/syscalls with host adapters.
NET_TCP_STACK_SRCS := src/net/net_tcp.c src/net/net_tcp_syscall.c src/net/tcp_tcb.c src/net/tcp.c
NET_SOCKET_HOST_SRCS := tests/net_socket_host.c tests/net_lock_host.c src/kernel/handoff.c src/net/net_udp_owner.c src/net/net_socket.c src/net/net_socket_syscall.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c $(NET_TCP_STACK_SRCS)

.PHONY: test-network-smp-phase2b-host test-network-smp-phase2b
test-network-smp-phase2b-host:
	@python3 scripts/test_network_smp_phase2b_host.py
test-network-smp-phase2b: test-network-smp-phase2b-host
	@python3 scripts/test_network_smp_phase2b.py
.PHONY: test-net-tcp-socket-host
.PHONY: test-net-tcp-client
.PHONY: test-supervisor-wait
test-supervisor-wait: $(BOOTABLE_ISO)
	@python3 scripts/test_supervisor_wait.py
.PHONY: test-net-tcp-server
.PHONY: test-net-tcp-fixture test-net-nc-host test-net-tcp-matrix test-net-tcp-retention test-net-tcp-synthetic
test-net-tcp-fixture:
	@python3 tests/test_net_tcp_fixture.py
test-net-nc-boundaries: $(BOOTABLE_ISO) test-net-nc-host
	@python3 scripts/test_net_nc_boundaries.py

test-net-nc-data-fin: $(BOOTABLE_ISO) test-net-tcp-socket-host test-net-nc-host
	@python3 scripts/test_net_nc_data_fin.py

.PHONY: test-net-nc-data-fin

.PHONY: test-net-nc-boundaries

.PHONY: test-perm-creds-host
.PHONY: test-process-registry-host
test-process-registry-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -DTEST_SMP_MEMORY -DTEST_PROCESS_REGISTRY_AUDIT -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/kernel tests/process_registry_audit_host.c src/kernel/process_table.c src/kernel/creds.c -o $(BUILD_DIR)/process_registry_audit_host
	@timeout 15s $(BUILD_DIR)/process_registry_audit_host

test-perm-creds-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/perm_creds_host.c src/kernel/creds.c -o $(BUILD_DIR)/perm_creds_host
	@$(BUILD_DIR)/perm_creds_host

test-net-nc-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/net_nc_host.c user/dns.c user/dns_codec.c user/netconf.c -o $(BUILD_DIR)/net_nc_host
	@$(BUILD_DIR)/net_nc_host
test-net-tcp-matrix: $(BOOTABLE_ISO) test-net-tcp-fixture test-net-nc-host test-net-tcp-socket-host
	@python3 scripts/test_net_tcp_matrix.py --all
.PHONY: test-net-tcp
test-net-tcp: test-net-tcp-matrix
test-net-tcp-retention: $(BOOTABLE_ISO)
	@python3 scripts/test_net_tcp_matrix.py --retention
test-net-tcp-synthetic: $(BOOTABLE_ISO) test-net-tcp-fixture
	@python3 scripts/test_net_tcp_matrix.py --core
	@python3 scripts/test_net_tcp_matrix.py --backlog
test-net-tcp-server: $(BOOTABLE_ISO) test-net-tcp-host test-net-tcp-tcb-host test-net-tcp-socket-host
	@$(PYTHON) scripts/test_net_tcp_server.py
test-net-tcp-client: $(BOOTABLE_ISO) test-net-tcp-host test-net-tcp-tcb-host test-net-tcp-socket-host
	@python3 scripts/test_net_tcp_client.py
test-net-tcp-socket-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/fs $(subst tests/net_socket_host.c,tests/net_tcp_owner_host.c,$(NET_SOCKET_HOST_SRCS)) -Wl,--wrap=net_tcp_send -o $(BUILD_DIR)/net_tcp_socket_host
	@$(BUILD_DIR)/net_tcp_socket_host
	@python3 tests/test_tcp_deadline_guard.py
.PHONY: ext4-fixtures
ext4-fixtures:
	@python3 scripts/create_ext4_fixtures.py

.PHONY: test-ext4-format-host
test-ext4-format-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_format_host.c -o $(BUILD_DIR)/ext4_format_host
	@python3 scripts/test_ext4_format_host.py

.PHONY: test-wget-host test-wget test-wget-https-host
test-wget-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include user/wget_codec.c tests/wget_host.c -o $(BUILD_DIR)/wget_host
	@$(BUILD_DIR)/wget_host

test-wget-https-host: test-tls-unit-host
	@python3 scripts/test_wget_https.py

test-wget: $(BOOTABLE_ISO) $(NVME_GPT_IMG) test-wget-host test-wget-https-host
	@python3 scripts/test_wget.py

.PHONY: test-dhcpc-host test-dhcpc-v2-host test-dhcp-probe
test-dhcpc-v2-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Iuser user/dhcp_codec.c tests/dhcpc_v2_host.c -o $(BUILD_DIR)/dhcpc_v2_host
	@$(BUILD_DIR)/dhcpc_v2_host

test-dhcpc-host: test-dhcpc-v2-host
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Iuser user/dhcp_codec.c tests/dhcpc_host.c -o $(BUILD_DIR)/dhcpc_host
	@$(BUILD_DIR)/dhcpc_host

test-dhcp-probe: $(BOOTABLE_ISO) test-dhcpc-host
	@python3 scripts/test_dhcp_probe.py

.PHONY: test-dhcpc test-dhcpc-v2
test-dhcpc: $(BOOTABLE_ISO) test-dhcpc-host
	@python3 scripts/test_dhcpc.py

test-dhcpc-v2: $(BOOTABLE_ISO) test-dhcpc-host
	@python3 scripts/test_dhcpc_v2.py

.PHONY: test-net-dns-host
test-net-dns-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/net_dns_host.c user/dns_codec.c -o $(BUILD_DIR)/net_dns_host
	@$(BUILD_DIR)/net_dns_host
	@python3 tests/test_dns_fixture.py
	@python3 tests/test_dns_lan_peer.py
.PHONY: test-net-dns test-net-dns-lifecycle
.PHONY: test-net-link
test-net-link: $(BOOTABLE_ISO) test-net-i219-host test-net-rings-host test-net-eth-host test-net-ipv4-host test-net-tcp-socket-host
	@python3 scripts/test_net_link.py
.PHONY: test-net-tcp-deadlines
test-net-tcp-deadlines: $(BOOTABLE_ISO) test-net-tcp-socket-host
	@python3 scripts/test_net_tcp_deadlines.py
test-net-dns: $(BOOTABLE_ISO) test-net-dns-host test-net-tcp-socket-host
# User-backend DNS fixture needs loopback UDP/TCP port 53 bind permission.
	@python3 scripts/test_net_dns.py --all
test-net-dns-lifecycle: $(BOOTABLE_ISO) test-net-dns-host test-net-tcp-socket-host
	@python3 scripts/test_net_dns_lifecycle.py
test-net-udp: $(BOOTABLE_ISO) test-net-udp-host test-net-socket-host
	@python3 scripts/test_net_udp.py
.PHONY: test-net-udp
test-net-udp-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net tests/net_udp_host.c src/net/udp.c src/net/checksum.c -o $(BUILD_DIR)/net_udp_host
	@$(BUILD_DIR)/net_udp_host

# NET-2 checkpoint A: pure codec only; no live TCP delivery is enabled.
.PHONY: test-net-tcp-host
test-net-tcp-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net tests/net_tcp_host.c src/net/tcp.c src/net/checksum.c -o $(BUILD_DIR)/net_tcp_host
	@$(BUILD_DIR)/net_tcp_host

$(BUILD_DIR)/net/tcp.o: CFLAGS += -Os -Wframe-larger-than=512 -fstack-usage

.PHONY: test-net-tcp-tcb-host
test-net-tcp-tcb-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net tests/net_tcp_tcb_host.c src/net/tcp_tcb.c src/net/tcp.c src/net/checksum.c -o $(BUILD_DIR)/net_tcp_tcb_host
	@$(BUILD_DIR)/net_tcp_tcb_host

$(BUILD_DIR)/net/tcp_tcb.o: CFLAGS += -Os -Wframe-larger-than=512 -fstack-usage
$(BUILD_DIR)/net/net_tcp.o $(BUILD_DIR)/net/net_tcp_syscall.o: CFLAGS += -Os -Wframe-larger-than=512 -fstack-usage
test-net-socket-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/fs $(NET_SOCKET_HOST_SRCS) -o $(BUILD_DIR)/net_socket_host
	@$(BUILD_DIR)/net_socket_host

# Networking Phase 0: Pure host tests for checksum, Ethernet, ARP, IPv4, and pbuf
.PHONY: test-net-host
test-net-icmp-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net tests/net_icmp_host.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c -o $(BUILD_DIR)/net_icmp_host
	@$(BUILD_DIR)/net_icmp_host

.PHONY: test-net-icmp-host
test-net-ipv4-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/arch/x86_64 -Isrc/drivers tests/net_ipv4_host.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c -o $(BUILD_DIR)/net_ipv4_host
	@$(BUILD_DIR)/net_ipv4_host
.PHONY: test-net-ipv4-host
test-net-ping-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_ping_host.c tests/net_lock_host.c src/net/net_ping.c -o $(BUILD_DIR)/net_ping_host
	@$(BUILD_DIR)/net_ping_host
.PHONY: test-net-ping-host
.PHONY: test-net-trace-host test-net-trace
test-net-trace-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DNET_TRACE_HOST_TEST -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_trace_host.c tests/net_lock_host.c src/net/net_ping.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c -o $(BUILD_DIR)/net_trace_host
	@$(BUILD_DIR)/net_trace_host
	@$(CC) -O1 -g -DTOOL_HOST_TEST -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser/tools tests/traceroute_host.c user/traceroute.c user/tools/common.c -o $(BUILD_DIR)/traceroute_host
	@$(BUILD_DIR)/traceroute_host
test-net-trace: $(BOOTABLE_ISO) $(BUILD_DIR)/trace_probe.elf test-net-trace-host
	@python3 scripts/test_net_trace.py
test-net-icmp: $(BOOTABLE_ISO) test-net-icmp-host test-net-ipv4-host test-net-ping-host
	@python3 scripts/test_net_icmp.py
.PHONY: test-net-icmp
test-net-eth-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_eth_host.c tests/net_lock_host.c src/net/net.c src/net/net_control.c src/kernel/handoff.c src/net/net_ping.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c src/net/arp.c -o $(BUILD_DIR)/net_eth_host
	@$(BUILD_DIR)/net_eth_host

.PHONY: test-net-eth-host test-net-eth
test-net-eth: $(BOOTABLE_ISO) test-net-eth-host
	@python3 scripts/test_net_eth.py

test-net-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net tests/net_host.c src/net/checksum.c src/net/eth.c src/net/arp.c src/net/ipv4.c -o $(BUILD_DIR)/net_host
	@$(BUILD_DIR)/net_host

test-net-ifconfig-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_ifconfig_host.c tests/net_lock_host.c src/net/net.c src/net/net_control.c src/kernel/handoff.c src/net/net_ping.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c src/net/arp.c -o $(BUILD_DIR)/net_ifconfig_host
	@$(BUILD_DIR)/net_ifconfig_host

.PHONY: test-net-ifconfig-host test-net-ifconfig test-net-ifup
test-net-ifconfig: $(BOOTABLE_ISO) test-net-ifconfig-host
	@python3 scripts/test_net_ifconfig.py

test-net-ifup: $(BOOTABLE_ISO) test-net-ifconfig-host
	@python3 scripts/test_net_ifup.py

# Isolated installed-root BIOS/UEFI SMP=1/4; disposable NVMe regular files.
.PHONY: test-net-installed
test-net-installed: test-net-ifconfig-host
	@python3 scripts/test_net_installed.py

# Networking Phase 1a: QEMU PCI discovery, MMIO mapping, MAC and STATUS registers
.PHONY: test-net-pci
test-net-pci: $(BOOTABLE_ISO)
	@python3 scripts/test_net_pci.py



# NET Phase 2a: polled descriptor DMA, raw TX/RX and pcap byte audit.
.PHONY: test-net-rings
test-net-rings: $(BOOTABLE_ISO) test-net-rings-host
	@python3 scripts/test_net_rings.py

.PHONY: test-net-rings-host
test-net-rings-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -g -fsanitize=address,undefined -Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread -Isrc/include -Isrc/drivers -Isrc/mm -Isrc/arch/x86_64 -Isrc/lib tests/net_rings_host.c -Wl,--gc-sections -o $(BUILD_DIR)/net_rings_host
	@$(BUILD_DIR)/net_rings_host

# NET Phase 2b: PCH init/reset/stop discipline with deterministic hardware mocks.
.PHONY: test-net-i219-host
test-net-i219-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -g -fsanitize=address,undefined -Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread -Isrc/include -Isrc/drivers -Isrc/mm -Isrc/arch/x86_64 -Isrc/lib tests/net_i219_host.c -Wl,--gc-sections -o $(BUILD_DIR)/net_i219_host
	@$(BUILD_DIR)/net_i219_host

.PHONY: test-ext4-read-host test-ext4-read
test-ext4-read-host: test-ext4-format-host

test-ext4-read: all
	python3 scripts/test_ext4_read.py

.PHONY: test-ext4-alloc-host
test-ext4-alloc-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_alloc_host.c -o $(BUILD_DIR)/ext4_alloc_host
	@python3 scripts/test_ext4_alloc_host.py

.PHONY: test-ext4-write-host
test-ext4-write-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_write_host.c -o $(BUILD_DIR)/ext4_write_host
	@python3 scripts/test_ext4_write_host.py

.PHONY: test-ext4-write
test-ext4-write: all
	@python3 scripts/test_ext4_write.py

.PHONY: image-ext4
image-ext4: all
	@python3 scripts/create_ext4_boot_img.py --iso-root $(ISO_ROOT) --limine-dir $(LIMINE_DIR)
	@python3 scripts/prepare_ext4_dell.py

.PHONY: test-ext4-usb
test-ext4-usb: all
	@python3 scripts/test_ext4_usb.py

.PHONY: test-jbd2-replay-host
test-jbd2-replay-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/jbd2_replay_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/jbd2_replay_host
	@python3 scripts/test_jbd2_replay_host.py

.PHONY: test-ext4-namespace-host
.PHONY: test-ext4-orphan-host
.PHONY: test-ext4-mount-host
.PHONY: test-ext4-journal-mount
.PHONY: test-ext4-mount-smoke-host
EXT4_MOUNT_EVIDENCE ?= .codex-remote-attachments/ext4-phase8-5
EXT4_INTEGRATION_EVIDENCE ?= .codex-remote-attachments/ext4-phase8-6
EXT4_CRASH_EVIDENCE ?= .codex-remote-attachments/ext4-phase9
.PHONY: test-ext4-crash-model-host
.PHONY: test-ext4-guest-crash
.PHONY: test-ext4-journal-usb test-ext4-journal-usb-admission test-ext4-journal-usb-crash test-ext4-journal-usb-fault
# Prepared with create_ext4_guest_workspace.py --usb-journal; no shared build.
test-ext4-journal-usb:
	@test -n "$(EXT4_USB_JOURNAL_WORKSPACE)" || (echo "Set EXT4_USB_JOURNAL_WORKSPACE to the prepared isolated snapshot"; exit 1)
	python3 scripts/test_ext4_journal_usb.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --matrix --integration
test-ext4-journal-usb-admission:
	@test -n "$(EXT4_USB_JOURNAL_WORKSPACE)" || (echo "Set EXT4_USB_JOURNAL_WORKSPACE"; exit 1)
	python3 scripts/test_ext4_usb_journal_admission.py "$(EXT4_USB_JOURNAL_WORKSPACE)"
test-ext4-journal-usb-crash:
	@test -n "$(EXT4_USB_JOURNAL_WORKSPACE)" || (echo "Set EXT4_USB_JOURNAL_WORKSPACE"; exit 1)
	python3 scripts/test_ext4_guest_crash.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --usb --matrix
	python3 scripts/test_ext4_guest_crash.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --usb --matrix --recovery-only
	python3 scripts/test_ext4_guest_append_crash.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --usb
	python3 scripts/test_ext4_guest_orphan_crash.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --usb
test-ext4-journal-usb-fault:
	@test -n "$(EXT4_USB_JOURNAL_WORKSPACE)" || (echo "Set EXT4_USB_JOURNAL_WORKSPACE"; exit 1)
	python3 scripts/test_ext4_usb_journal_fault.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --calibrate
	python3 scripts/test_ext4_usb_journal_fault.py "$(EXT4_USB_JOURNAL_WORKSPACE)" --matrix

# Prepared isolated workspace only: never rebuild the shared boot artifacts.
test-ext4-guest-crash:
	@test -n "$(EXT4_GUEST_WORKSPACE)" || (echo "Set EXT4_GUEST_WORKSPACE to a prepared isolated snapshot"; exit 1)
	python3 scripts/test_ext4_guest_crash.py "$(EXT4_GUEST_WORKSPACE)" --matrix
	python3 scripts/test_ext4_guest_crash.py "$(EXT4_GUEST_WORKSPACE)" --matrix --recovery-only
	python3 scripts/test_ext4_guest_append_crash.py "$(EXT4_GUEST_WORKSPACE)"
	python3 scripts/test_ext4_guest_orphan_crash.py "$(EXT4_GUEST_WORKSPACE)"

.PHONY: test-ext4-crash-host
.PHONY: test-ext4-crash-controls-host test-ext4-crash-fragment-host
test-ext4-crash-controls-host:
	@mkdir -p $(EXT4_CRASH_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_crash_bootstrap_host.c tests/ext4_fault_disk.c -o $(EXT4_CRASH_EVIDENCE)/bin/ext4_crash_bootstrap_host
	@FORTRESS_EXT4_CRASH_EVIDENCE=$(EXT4_CRASH_EVIDENCE) python3 scripts/test_ext4_crash_bootstrap.py $(EXT4_CRASH_INVENTORY)
	@FORTRESS_EXT4_CRASH_EVIDENCE=$(EXT4_CRASH_EVIDENCE) python3 scripts/test_ext4_crash_recovery_oracle.py $(EXT4_CRASH_INVENTORY)
	@python3 scripts/test_ext4_crash_oracle.py $(EXT4_CRASH_INVENTORY)
test-ext4-crash-fragment-host:
	@mkdir -p $(EXT4_CRASH_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_crash_fragment_host.c tests/ext4_fault_disk.c -o $(EXT4_CRASH_EVIDENCE)/bin/ext4_crash_fragment_host
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_crash_inventory_host.c tests/ext4_fault_disk.c -o $(EXT4_CRASH_EVIDENCE)/bin/ext4_crash_inventory_host
	@FORTRESS_EXT4_CRASH_EVIDENCE=$(EXT4_CRASH_EVIDENCE) python3 scripts/test_ext4_crash_fragment.py $(EXT4_INTEGRATION_FIXTURES)
test-ext4-crash-host:
	@mkdir -p $(EXT4_CRASH_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_crash_recover_host.c tests/ext4_fault_disk.c -o $(EXT4_CRASH_EVIDENCE)/bin/ext4_crash_recover_host
	@python3 scripts/test_ext4_crash_sparse.py
	@FORTRESS_EXT4_CRASH_EVIDENCE=$(EXT4_CRASH_EVIDENCE) python3 scripts/test_ext4_crash_recovery.py $(EXT4_CRASH_INVENTORY)
test-ext4-crash-model-host:
	@mkdir -p $(EXT4_CRASH_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_crash_inventory_host.c tests/ext4_fault_disk.c -o $(EXT4_CRASH_EVIDENCE)/bin/ext4_crash_inventory_host
	@FORTRESS_EXT4_CRASH_EVIDENCE=$(EXT4_CRASH_EVIDENCE) python3 scripts/test_ext4_crash_inventory.py $(EXT4_INTEGRATION_FIXTURES)
.PHONY: test-ext4-integration-host
.PHONY: test-ext4-integration-staging-host
test-ext4-integration-staging-host:
	@mkdir -p $(EXT4_INTEGRATION_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_integration_host.c tests/ext4_fault_disk.c -o $(EXT4_INTEGRATION_EVIDENCE)/bin/ext4_integration_staging
	@FORTRESS_EXT4_INTEGRATION_EVIDENCE=$(EXT4_INTEGRATION_EVIDENCE) python3 scripts/test_ext4_integration_host.py $(EXT4_INTEGRATION_FIXTURES) --staging
.PHONY: test-ext4-integration
test-ext4-integration: all
	@FORTRESS_EXT4_INTEGRATION_EVIDENCE=$(EXT4_INTEGRATION_EVIDENCE) FORTRESS_EXT4_INTEGRATION_SMP=1 python3 scripts/test_ext4_integration.py $(EXT4_INTEGRATION_FIXTURES)
	@FORTRESS_EXT4_INTEGRATION_EVIDENCE=$(EXT4_INTEGRATION_EVIDENCE) FORTRESS_EXT4_INTEGRATION_SMP=4 python3 scripts/test_ext4_integration.py $(EXT4_INTEGRATION_FIXTURES)

test-ext4-integration-host:
	@mkdir -p $(EXT4_INTEGRATION_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_integration_host.c tests/ext4_fault_disk.c -o $(EXT4_INTEGRATION_EVIDENCE)/bin/ext4_integration_host
	@FORTRESS_EXT4_INTEGRATION_EVIDENCE=$(EXT4_INTEGRATION_EVIDENCE) python3 scripts/test_ext4_integration_host.py $(EXT4_INTEGRATION_FIXTURES)

test-ext4-mount-smoke-host:
	@mkdir -p $(EXT4_MOUNT_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_mount_host.c tests/ext4_fault_disk.c -o $(EXT4_MOUNT_EVIDENCE)/bin/ext4_mount_host_smoke
	@FORTRESS_EXT4_EVIDENCE=$(EXT4_MOUNT_EVIDENCE) python3 scripts/test_ext4_mount_host.py --smoke $(EXT4_MOUNT_FIXTURES)

test-ext4-journal-mount: all
	@FORTRESS_EXT4_EVIDENCE=$(EXT4_MOUNT_EVIDENCE) python3 scripts/test_ext4_journal_mount.py

test-ext4-mount-host:
	@mkdir -p $(EXT4_MOUNT_EVIDENCE)/bin
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_mount_host.c tests/ext4_fault_disk.c -o $(EXT4_MOUNT_EVIDENCE)/bin/ext4_mount_host
	@FORTRESS_EXT4_EVIDENCE=$(EXT4_MOUNT_EVIDENCE) python3 scripts/test_ext4_mount_host.py

.PHONY: test-ext4-orphan-deep-host
test-ext4-orphan-deep-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_orphan_deep_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/ext4_orphan_deep_host
	@python3 scripts/test_ext4_orphan_deep_host.py $(EXT4_ORPHAN_DEEP_ARGS)

test-ext4-orphan-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_orphan_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/ext4_orphan_host
	@python3 scripts/test_ext4_orphan_host.py $(EXT4_ORPHAN_ARGS)

test-ext4-namespace-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_namespace_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/ext4_namespace_host
	@python3 scripts/test_ext4_namespace_host.py

.PHONY: test-ext4-journal-file-host
test-ext4-journal-file-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_journal_file_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/ext4_journal_file_host
	@python3 scripts/test_ext4_journal_file_host.py

.PHONY: test-ext4-transaction-host
test-ext4-transaction-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_transaction_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/ext4_transaction_host
	@python3 scripts/test_ext4_transaction_host.py

.PHONY: test-jbd2-write-host
test-jbd2-write-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/jbd2_write_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/jbd2_write_host
	@python3 scripts/test_jbd2_write_host.py

.PHONY: test-perm-fs-host
.PHONY: test-perm-matrix-host test-perm-create-host
test-perm-matrix-host test-perm-create-host:
	@python3 scripts/test_perm_values.py

.PHONY: test-perm-admission-host
.PHONY: test-perm-runfs-host
.PHONY: test-perm-filesystems-host
.PHONY: test-perm-syscalls-host
.PHONY: test-perm-privileges-host
test-perm-privileges-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O0 -g -DTEST_SMP_MEMORY -DTEST_PERMISSIONS_VALUES -DTEST_PERMISSIONS_ENFORCEMENT -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/kernel tests/perm_signal_enforcement_host.c src/kernel/process_table.c src/kernel/creds.c -o $(BUILD_DIR)/perm_signal_enforcement_host
	@$(BUILD_DIR)/perm_signal_enforcement_host
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -DTEST_PERMISSIONS_VALUES -DTEST_PERMISSIONS_ENFORCEMENT -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/fs $(NET_SOCKET_HOST_SRCS) src/kernel/creds.c -o $(BUILD_DIR)/perm_bind_enforcement_host
	@$(BUILD_DIR)/perm_bind_enforcement_host
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm -Isrc/kernel -Isrc/arch/x86_64 tests/perm_power_enforcement_host.c src/kernel/creds.c src/fs/permission_values.c -o $(BUILD_DIR)/perm_power_enforcement_host
	@$(BUILD_DIR)/perm_power_enforcement_host

test-perm-syscalls-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm -Isrc/kernel -Isrc/arch/x86_64 tests/perm_syscalls_host.c src/kernel/creds.c -o $(BUILD_DIR)/perm_syscalls_host
	@$(BUILD_DIR)/perm_syscalls_host

test-perm-filesystems-host:
	@python3 scripts/test_perm_enforcement_host.py "$(PERM_FIXTURE_DIR)"

$(BUILD_DIR)/perm_phase2_user.elf: tests/perm_phase2_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -DTEST_PERMISSIONS_ENFORCEMENT -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $(BUILD_DIR)/perm_phase2_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/perm_phase2_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/perm_phase2_user_start.o $(BUILD_DIR)/perm_phase2_user.o -o $@

test-perm-runfs-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/perm_runfs_enforcement_host.c src/fs/permission_values.c src/kernel/creds.c -o $(BUILD_DIR)/perm_runfs_enforcement_host
	@$(BUILD_DIR)/perm_runfs_enforcement_host

test-etcfs-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/etcfs_host.c src/kernel/creds.c -o $(BUILD_DIR)/etcfs_host
	@$(BUILD_DIR)/etcfs_host

test-etcfs-live: bin/fortress.iso
	@python3 scripts/test_etcfs_live.py

test-perm-admission-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/perm_admission_host.c -o $(BUILD_DIR)/perm_admission_host
	@$(BUILD_DIR)/perm_admission_host
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm -Isrc/kernel -Isrc/arch/x86_64 tests/perm_readdir_syscall_host.c src/kernel/creds.c -o $(BUILD_DIR)/perm_readdir_syscall_host
	@$(BUILD_DIR)/perm_readdir_syscall_host

test-perm-wiring-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/perm_wiring_host.c -o $(BUILD_DIR)/perm_wiring_host
	@$(BUILD_DIR)/perm_wiring_host

.PHONY: test-perm-wiring-host test-perm-signal-wiring-host test-perm-capability-host
.PHONY: test-perm-phase1
test-perm-phase1:
	@PERM_PHASE1_FIXTURE="$(PERM_PHASE1_FIXTURE)" python3 scripts/test_perm_phase1.py
test-perm-signal-wiring-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O0 -g -DTEST_SMP_MEMORY -DTEST_PERMISSIONS_VALUES -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -Isrc/include -Isrc/kernel tests/perm_signal_wiring_host.c src/kernel/process_table.c src/kernel/creds.c -o $(BUILD_DIR)/perm_signal_wiring_host
	@$(BUILD_DIR)/perm_signal_wiring_host
test-perm-capability-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -DTEST_PERMISSIONS_VALUES -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/fs $(NET_SOCKET_HOST_SRCS) -o $(BUILD_DIR)/perm_capability_host
	@$(BUILD_DIR)/perm_capability_host
	@python3 scripts/test_perm_power_host.py

test-perm-fs-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/perm_fs_host.c -o $(BUILD_DIR)/perm_fs_host
	@$(BUILD_DIR)/perm_fs_host

$(BUILD_DIR)/perm_user.elf: tests/perm_user.c user/shell_start.asm user/shell.ld src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c tests/perm_user.c -o $(BUILD_DIR)/perm_user.o
	@$(AS) -f elf64 user/shell_start.asm -o $(BUILD_DIR)/perm_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/perm_user_start.o $(BUILD_DIR)/perm_user.o -o $@

.PHONY: test-perm-inode-host test-perm-phase0-guest
test-perm-inode-host:
	@python3 scripts/test_perm_inodes.py

test-perm-phase0-guest: $(BUILD_DIR)/perm_user.elf $(KERNEL_ELF) $(BOOTABLE_ISO)
	@python3 scripts/test_perm_guest.py $(PERM_INODE_FIXTURE) $(PERM_GUEST_ARGS)

.PHONY: test-perm-device-host
test-perm-device-host:
	@test -n "$(PERM_DEVICE_FIXTURE)" || (echo "Set PERM_DEVICE_FIXTURE to a disposable EXT4 source image"; exit 1)
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/perm_device_host.c tests/ext4_fault_disk.c -o $(BUILD_DIR)/perm_device_host
	@$(BUILD_DIR)/perm_device_host $(PERM_DEVICE_FIXTURE)

.PHONY: test-perm-spawn-host test-sudo test-su-host
.PHONY: test-perm-phase5-host
test-perm-phase5-host:
	@python3 scripts/test_perm_phase5_host.py
test-su-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -no-pie -Isrc/include -Isrc/fs -Iuser/tools -DTOOL_HOST_TEST tests/su_host.c user/tools/common.c user/tools/userdb.c user/tools/digest.c -lcrypt -o $(BUILD_DIR)/su_host
	@$(BUILD_DIR)/su_host
test-perm-spawn-host: test-su-host
	@python3 scripts/test_perm_spawn_host.py
test-sudo:
	@python3 scripts/test_sudo.py

$(BUILD_DIR)/perm_phase4_user.elf: tests/perm_phase4_user.c user/entry_security.h user/tools/common.h user/permissions_cli.h $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $(BUILD_DIR)/perm_phase4_user.o
	@$(AS) -f elf64 -DTOOL_ENTRY=phase4_main user/tools/start.asm -o $(BUILD_DIR)/perm_phase4_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/perm_phase4_user_start.o $(BUILD_DIR)/perm_phase4_user.o $(BUILD_DIR)/tool-common.o -o $@

$(USER_SH_BUILTIN_ELF): user/entry_security.h

# Phase 0 only: fresh regular-file NVMe fixtures and firmware-loader evidence.
.PHONY: test-root-phase0
test-root-phase0: $(KERNEL_ELF) $(INITRAMFS_TAR)
	@python3 scripts/test_root_phase0.py

ifeq ($(ROOT_PHASE1_TEST),1)
CFLAGS += -DTEST_ROOT_PHASE1
endif
ifeq ($(ROOT_PHASE2_TEST),1)
CFLAGS += -DTEST_ROOT_PHASE2
endif

.PHONY: test-root-phase1-host test-root-phase1
test-root-phase1-host: $(KERNEL_ELF) $(INITRAMFS_TAR)
	@python3 scripts/test_root_phase1.py --host-only
test-root-phase1: $(KERNEL_ELF) $(INITRAMFS_TAR)
	@python3 scripts/test_root_phase1.py

.PHONY: installer-image installer
installer: installer-image
# Full installer image (bin/fortress-installer.img, bin/fortress-installer.iso)
installer-image: bin/fortress.elf bin/initramfs.tar build/tool-install.elf limine-setup
	@python3 scripts/create_installer_media.py

# Installer programs are staged by installer/combined packaging, not the base archive.
build/tool-install-multi.elf: user/tools/install.c src/include/install_abi.h src/include/terminal.h src/include/ssh_hostkey.h $(CRYPTO_LIB) build/tool-userdb.o build/tool-common.o build/tool-digest.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -DINSTALLER_MULTI -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c user/tools/install.c -o build/tool-install-multi.o
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=install_main user/tools/start.asm -o build/tool-install-multi-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld build/tool-install-multi-start.o build/tool-install-multi.o build/tool-userdb.o build/tool-common.o $(CRYPTO_LIB) -o $@

build/tool-install.elf: user/tools/install.c src/include/install_abi.h src/include/terminal.h src/include/ssh_hostkey.h $(CRYPTO_LIB) build/tool-userdb.o build/tool-common.o build/tool-digest.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c user/tools/install.c -o build/tool-install.o
	@$(AS) -f elf64 -DTOOL_ENTRY=install_main user/tools/start.asm -o build/tool-install-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld build/tool-install-start.o build/tool-install.o build/tool-userdb.o build/tool-common.o $(CRYPTO_LIB) -o $@

build/tool-install-inspect.elf: user/tools/install_inspect.c src/include/install_abi.h build/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o build/tool-install-inspect.o
	@$(AS) -f elf64 -DTOOL_ENTRY=install_main user/tools/start.asm -o build/tool-install-inspect-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld build/tool-install-inspect-start.o build/tool-install-inspect.o build/tool-common.o -o $@

build/tool-install-preview.elf: user/tools/install.c src/include/install_abi.h src/include/terminal.h build/tool-userdb.o build/tool-common.o build/tool-digest.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -DINSTALLER_PREVIEW -Os -fno-pie -fno-asynchronous-unwind-tables -c user/tools/install.c -o build/tool-install-preview.o
	@$(AS) -f elf64 -DTOOL_ENTRY=install_main user/tools/start.asm -o build/tool-install-preview-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld build/tool-install-preview-start.o build/tool-install-preview.o build/tool-userdb.o build/tool-digest.o build/tool-common.o -o $@

.PHONY: test-install-runtime-host
test-install-runtime-host: bin/fortress.elf bin/initramfs.tar
	@python3 scripts/test_install_runtime.py

.PHONY: test-install-cli-host test-root-phase4-host
.PHONY: test-install-log-host test-install-log test-install-var
test-install-var:
	@test -n "$(INSTALLER_DISK)" || (echo "Set INSTALLER_DISK to a disposable installed image under build/root-phase4"; exit 1)
	@python3 scripts/test_installer_var.py --disk "$(INSTALLER_DISK)"
test-install-log-host:
	@test -n "$(INSTALLER_MEDIA)" || { echo 'Set INSTALLER_MEDIA to retained multi installer media'; exit 2; }
	@python3 scripts/test_install_log.py --media "$(INSTALLER_MEDIA)"
test-install-log:
	@test -n "$(INSTALLER_MEDIA)" || { echo 'Set INSTALLER_MEDIA to retained multi installer media'; exit 2; }
	@python3 scripts/test_installer_logging.py --media "$(INSTALLER_MEDIA)"
.PHONY: test-install-confirmation
test-install-confirmation:
	@test -n "$(INSTALLER_MEDIA)" || { echo 'Set INSTALLER_MEDIA to retained installer media'; exit 2; }
	@python3 scripts/test_installer_confirmation.py --media "$(INSTALLER_MEDIA)"
test-install-cli-host:
	@python3 scripts/test_install_cli.py

test-root-phase4-host: test-install-control-host test-install-payload-host test-install-fat-host test-install-provision-host test-install-runtime-host test-install-cli-host test-heap-memory-host test-root-boot-host

.PHONY: test-root-phase4
# Audited media workspace from create_installer_media.py, never a device path.
test-root-phase4:
	@test -n "$(INSTALLER_MEDIA)" || { echo 'Set INSTALLER_MEDIA to the retained build/root-phase4/media-* workspace'; exit 2; }
	@python3 scripts/test_installer.py --media "$(INSTALLER_MEDIA)"

.PHONY: test-root-phase4-multi
# Two-drive fixtures and source-free installed boot; regular files only.
test-root-phase4-multi:
	@test -n "$(INSTALLER_MEDIA)" || { echo 'Set INSTALLER_MEDIA to the retained multi-profile media workspace'; exit 2; }
	@python3 scripts/test_installer_multi.py --media "$(INSTALLER_MEDIA)"

# Phase 2A only: control/probes, isolated installed roots, no AP socket rollout.
.PHONY: test-network-smp-phase2a-host test-network-smp-phase2a
test-network-smp-phase2a-host:
	@python3 scripts/test_network_smp_phase2a_host.py
test-network-smp-phase2a: test-network-smp-phase2a-host
	@python3 scripts/test_network_smp_phase2a.py

.PHONY: test-network-smp-phase2c-host test-network-smp-phase2c
test-network-smp-phase2c-host:
	@python3 scripts/test_network_smp_phase2c_host.py
test-network-smp-phase2c: test-network-smp-phase2c-host
	@python3 scripts/test_network_smp_phase2c.py

.PHONY: test-network-smp-phase2d-host test-network-smp-phase2d
test-network-smp-phase2d-host:
	python3 scripts/test_network_smp_phase2d_host.py
test-network-smp-phase2d: test-network-smp-phase2d-host
	python3 scripts/test_network_smp_phase2d.py

.PHONY: test-sshd-sudo
test-sshd-sudo: $(BOOTABLE_ISO)
	python3 scripts/test_sshd_sudo.py --all

.PHONY: test-nano-ssh
test-nano-ssh: $(BOOTABLE_ISO)
	python3 scripts/test_nano_ssh.py bios
	python3 scripts/test_nano_ssh.py uefi

.PHONY: test-watchtower-boot
test-watchtower-boot: $(BOOTABLE_ISO)
	python3 scripts/test_watchtower_boot.py

.PHONY: test-watchtower-lifecycle
test-watchtower-lifecycle: $(BOOTABLE_ISO)
	python3 scripts/test_watchtower_lifecycle.py

.PHONY: test-watchtower-config-host
test-watchtower-config-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -DWATCHTOWER_HOST_TEST -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser/watchtower user/watchtower/watchtower_config.c tests/watchtower_config_host.c -o $(BUILD_DIR)/watchtower_config_host
	@$(BUILD_DIR)/watchtower_config_host

.PHONY: test-watchtower-ctl-host
test-watchtower-ctl-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -DWATCHTOWER_HOST_TEST -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser/watchtower user/watchtower/watchtower_service.c user/watchtower/watchtower_config.c user/watchtower/watchtower_ctl.c tests/watchtower_ctl_host.c -o $(BUILD_DIR)/watchtower_ctl_host
	@$(BUILD_DIR)/watchtower_ctl_host

.PHONY: test-watchtower-phase5-host
test-watchtower-phase5-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser tests/watchtower_phase5_host.c -o $(BUILD_DIR)/watchtower_phase5_host
	@$(BUILD_DIR)/watchtower_phase5_host

.PHONY: test-watchtower-host
test-watchtower-host: test-watchtower-config-host test-watchtower-ctl-host test-watchtower-phase5-host test-watchtower-identity-host
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -DWATCHTOWER_HOST_TEST -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser -Iuser/watchtower user/watchtower/watchtower_service.c user/watchtower/watchtower_config.c tests/watchtower_host.c -o $(BUILD_DIR)/watchtower_host
	@$(BUILD_DIR)/watchtower_host

.PHONY: test-watchtower-identity-host
test-watchtower-identity-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -no-pie -Wall -Wextra -Werror -Isrc/include tests/watchtower_identity_host.c user/watchtower/watchtower_identity.c user/tools/userdb.c user/tools/digest.c src/kernel/creds.c -o $(BUILD_DIR)/watchtower_identity_host
	@$(BUILD_DIR)/watchtower_identity_host

$(BUILD_DIR)/watchtower-identity-probe.elf: tests/watchtower_identity_probe.c src/include/syscall_abi.h $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -Wframe-larger-than=1024 -Isrc/include -Isrc/fs -Iuser/tools -c $< -o $(BUILD_DIR)/watchtower-identity-probe.o
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=identity_probe_main user/tools/start.asm -o $(BUILD_DIR)/watchtower-identity-start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/watchtower-identity-start.o $(BUILD_DIR)/watchtower-identity-probe.o $(BUILD_DIR)/tool-common.o -o $@

.PHONY: test-watchtower-identity
test-watchtower-identity: $(KERNEL_ELF) $(USER_WATCHTOWER_ELF) $(BUILD_DIR)/watchtower-identity-probe.elf
	python3 scripts/test_watchtower_identity.py

.PHONY: test-ssh-shutdown
test-ssh-shutdown: $(KERNEL_ELF) $(USER_WATCHTOWER_ELF) $(BUILD_DIR)/watchtower-identity-probe.elf $(BUILD_DIR)/sshd.elf
	python3 scripts/test_watchtower_lifecycle.py --staged

.PHONY: test-crond-host
test-crond-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -DCROND_HOST_TEST -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser/crond user/crond/crond.c tests/crond_host.c -o $(BUILD_DIR)/crond_host
	@$(BUILD_DIR)/crond_host

.PHONY: test-logrotate-host
test-logrotate-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -DLOGROTATE_HOST_TEST -Wall -Wextra -Werror -Isrc/include -Isrc/fs -Iuser/logrotate user/logrotate/logrotate.c tests/logrotate_host.c -o $(BUILD_DIR)/logrotate_host
	@$(BUILD_DIR)/logrotate_host

.PHONY: test-logging
test-logging: $(BOOTABLE_ISO) test-logger-host test-timeline-log-host test-boot-snapshot-host test-logrotate-host
	python3 scripts/test_logging.py




.PHONY: test-shell-scripting-host test-shell-scripting
test-shell-scripting-host:
	python3 scripts/test_shell_host.py
	python3 scripts/test_pipeline_host.py
	python3 scripts/test_s8_jobs_host.py
	python3 scripts/test_builtin_exec_host.py
	python3 scripts/test_runner_host.py

# Read-only base media inputs; only disposable test ISOs are generated.
SCRIPT_BASE ?= $(CURDIR)
test-shell-scripting: $(USER_SHELL_ELF) $(USER_SH_BUILTIN_ELF)
	python3 scripts/test_shell_scripting.py --base "$(SCRIPT_BASE)"

# UTC/local calendar support; logs retain UTC independent of TZ.
$(BUILD_DIR)/tool-date.o $(BUILD_DIR)/tool-logger.o $(BUILD_DIR)/dhcpc.o: user/timezone.h
$(BUILD_DIR)/timezone.o: user/timezone.c user/timezone.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -Wframe-larger-than=1024 -c $< -o $@
$(BUILD_DIR)/timezone_io.o: user/tools/timezone_io.c user/timezone.h user/tools/common.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -Wframe-larger-than=1024 -c $< -o $@
$(BUILD_DIR)/tool-date.elf: TIME_EXTRA = $(BUILD_DIR)/timezone.o $(BUILD_DIR)/timezone_io.o
$(BUILD_DIR)/tool-date.elf: $(BUILD_DIR)/timezone.o $(BUILD_DIR)/timezone_io.o
$(BUILD_DIR)/tool-logger.elf: TIME_EXTRA = $(BUILD_DIR)/timezone.o
$(BUILD_DIR)/tool-logger.elf: $(BUILD_DIR)/timezone.o
$(BUILD_DIR)/ntp.o: user/ntp.c user/ntp.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -c $< -o $@
$(BUILD_DIR)/ntpd.o: user/ntpd.c user/ntp.h user/dns.h user/udp_common.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -Wframe-larger-than=1024 -c $< -o $@
$(BUILD_DIR)/ntpd.elf: $(BUILD_DIR)/ntpd.o $(BUILD_DIR)/ntp.o $(DNS_OBJECTS) user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=ntpd_main user/tools/start.asm -o $(BUILD_DIR)/ntpd_start.o
	@$(LD) -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/ntpd_start.o $(BUILD_DIR)/ntpd.o $(BUILD_DIR)/ntp.o $(DNS_OBJECTS) -o $@

.PHONY: test-timezone-host
test-timezone-host: test-date-sleep-host test-date-host test-logger-host test-time-host
	@python3 scripts/test_timezone_host.py
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -Isrc/include tests/timezone_host.c user/timezone.c user/ntp.c -o $(BUILD_DIR)/timezone_host
	@$(BUILD_DIR)/timezone_host
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -Isrc/include -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/kernel -Isrc/fs tests/timekeeper_host.c -o $(BUILD_DIR)/timekeeper_host
	@$(BUILD_DIR)/timekeeper_host
	@gcc -std=c11 -Wall -Wextra -Werror -g -no-pie -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,--gc-sections -DTOOL_HOST_TEST -Isrc/include -Isrc/fs -Iuser tests/dhcp_timezone_host.c user/timezone.c user/tools/timezone_io.c user/dhcp_codec.c -o $(BUILD_DIR)/dhcp_timezone_host
	@$(BUILD_DIR)/dhcp_timezone_host

$(BUILD_DIR)/time-clock-probe.elf: tests/time_clock_probe.c user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -Wframe-larger-than=1024 -c $< -o $(BUILD_DIR)/time-clock-probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=time_clock_probe_main user/tools/start.asm -o $(BUILD_DIR)/time-clock-probe-start.o
	@$(LD) -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/time-clock-probe-start.o $(BUILD_DIR)/time-clock-probe.o -o $@

.PHONY: test-timezone
test-timezone: test-timezone-host test-dhcpc-host test-dhcpc-v2-host $(BUILD_DIR)/time-clock-probe.elf $(BUILD_DIR)/tool-date.elf $(BUILD_DIR)/tool-logger.elf $(BUILD_DIR)/ntpd.elf $(BUILD_DIR)/ntpd-test.elf $(USER_DHCPC_ELF) $(KERNEL_ELF)
	@python3 scripts/test_timezone.py

$(BUILD_DIR)/ntpd-test.elf: user/ntpd.c user/ntp.h $(BUILD_DIR)/ntp.o $(DNS_OBJECTS) user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -DTEST_NTP_PORT=43123 -Wframe-larger-than=1024 -c $< -o $(BUILD_DIR)/ntpd-test.o
	@$(AS) -f elf64 -DTOOL_ENTRY=ntpd_main user/tools/start.asm -o $(BUILD_DIR)/ntpd-test-start.o
	@$(LD) -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/ntpd-test-start.o $(BUILD_DIR)/ntpd-test.o $(BUILD_DIR)/ntp.o $(DNS_OBJECTS) -o $@

.PHONY: test-ext4-time-host
test-ext4-time-host:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_time_host.c tests/ext4_time_clock.c tests/ext4_fault_disk.c -o build/ext4_time_host
	python3 scripts/test_ext4_time_host.py

.PHONY: test-ext4-replace-host
test-ext4-replace-host:
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_replace_host.c tests/ext4_fault_disk.c src/fs/permission_values.c src/kernel/creds.c -o build/ext4_replace_host
	python3 scripts/test_ext4_replace_host.py

.PHONY: test-ext4-replace-fault-host
test-ext4-replace-fault-host:
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_replace_fault_host.c tests/ext4_time_clock.c tests/ext4_fault_disk.c src/fs/permission_values.c src/kernel/creds.c -o build/ext4_replace_fault_host
	python3 scripts/test_ext4_replace_fault_host.py

.PHONY: test-ext4-health-host
test-ext4-health-host:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/ext4_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_health_host.c tests/ext4_time_clock.c tests/ext4_fault_disk.c src/fs/permission_values.c src/kernel/creds.c -o build/ext4_health_host
	python3 scripts/test_ext4_health_host.py

.PHONY: test-ext4-health
test-ext4-health:
	python3 scripts/test_ext4_health.py

.PHONY: test-ext4-latency-host test-ext4-latency
test-ext4-latency-host:
	python3 scripts/test_ext4_latency_host.py

test-ext4-latency:
	python3 scripts/test_ext4_latency.py

.PHONY: test-ext4-stage-host test-ext4-stage test-ssh-stage-sla test-ext4-pkg-scale
test-ext4-stage-host:
	python3 scripts/test_ext4_stage_host.py
	python3 scripts/test_ext4_stage_syscall_host.py

test-ext4-stage:
	python3 scripts/test_ext4_latency.py --stage

.PHONY: test-ext4-mv-host test-ext4-mv
test-ext4-mv-host:
	python3 scripts/test_ext4_mv_host.py

test-ext4-mv:
	python3 scripts/test_ext4_mv.py

test-ssh-stage-sla:
	python3 scripts/test_ssh_stage_sla.py --firmware bios --smp 4
	python3 scripts/test_ssh_stage_sla.py --firmware uefi --smp 4

test-ext4-pkg-scale:
	python3 scripts/test_ext4_pkg_scale.py --firmware all --smp all



.PHONY: test-ssh-client-host test-ssh-client
test-ssh-client-host:
	@python3 scripts/test_ssh_client_host.py

test-ssh-client: $(BOOTABLE_ISO)
	@python3 scripts/test_ssh_client.py

.PHONY: test-pthread-host test-pthread
test-pthread-host: tests/pthread_host.c
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -pthread tests/pthread_host.c -o $(BUILD_DIR)/test-pthread-host
	@$(BUILD_DIR)/test-pthread-host

test-pthread: $(BOOTABLE_ISO)
	@python3 scripts/test_pthread.py

.PHONY: test-crash-dump-host test-crash-dump
test-crash-dump-host: tests/crash_dump_host.c
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -I. -Isrc/include -Isrc/kernel -Isrc/arch/x86_64 tests/crash_dump_host.c -o $(BUILD_DIR)/test-crash-dump-host
	@$(BUILD_DIR)/test-crash-dump-host

test-crash-dump: $(BOOTABLE_ISO)
	@python3 scripts/test_crash_dump.py

.PHONY: test-abi-version-host
test-abi-version-host: tests/abi_versioning_host.c
	@mkdir -p $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -no-pie -I. -Isrc/include -Isrc/kernel -Isrc/fs tests/abi_versioning_host.c -o $(BUILD_DIR)/test-abi-version-host
	@$(BUILD_DIR)/test-abi-version-host

# Private copied kernel and regular-file fixtures; never rebuild release media.
.PHONY: test-pkg-exec-boot
.PHONY: test-pkg-exec-cli-host
test-pkg-exec-cli-host:
	python3 scripts/test_pkg_exec_cli.py

test-pkg-exec-boot: scripts/test_pkg_exec_boot.py scripts/pkg_exec_overlay.py scripts/pkg_install_overlay.py tests/pkg_exec_kernel.inc tests/pkg_exec_ring3.c tests/pkg_exec_wire.h tests/pkg_exec_cli.c tests/pkg_exec_cli.h tests/pkg_install_fs.inc tests/pkg_plan_candidate.c user/pkg/mutation_cli.c
	@python3 -c "from pathlib import Path; assert Path('bin/initramfs.tar').is_file(), 'An existing read-only bootstrap is required; no release build is performed'"
	python3 scripts/test_pkg_exec_boot.py
