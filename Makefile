# FortressOS Build Automation
# Target: x86_64 UEFI Bare-Metal OS

SHELL := /bin/bash
.DELETE_ON_ERROR:

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
	@gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-pie -no-pie -Isrc/include -Isrc/kernel -Isrc/mm -Isrc/drivers tests/elf_loader_host.c src/kernel/elf.c -o build/elf-loader-host
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
$(BUILD_DIR)/wget.o: user/wget.c user/wget_codec.h user/dns.h user/dns_codec.h user/udp_common.h user/netconf.h src/include/socket_abi.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_WGET_ELF): $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=wget_main user/tools/start.asm -o $(BUILD_DIR)/wget_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/wget_start.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o -o $@
$(BUILD_DIR)/download.o: user/download.c user/wget.h user/udp_common.h src/include/syscall_abi.h src/include/socket_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(USER_DOWNLOAD_ELF): $(BUILD_DIR)/download.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=download_main user/tools/start.asm -o $(BUILD_DIR)/download_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/download_start.o $(BUILD_DIR)/download.o $(BUILD_DIR)/wget.o $(BUILD_DIR)/wget_codec.o $(DNS_OBJECTS) $(BUILD_DIR)/netconf.o -o $@
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
STREAM_TOOLS := cat head tail wc grep uniq xxd sort diff patch diskbench smpbench lockstat chmod chown id
LOGIN_TOOLS := login whoami sudo
LOGIN_ELFS := $(addprefix $(BUILD_DIR)/tool-,$(addsuffix .elf,$(LOGIN_TOOLS)))

ifeq ($(LOGIN_TEST),1)
CFLAGS += -DTEST_LOGIN_BOOT
endif

$(BUILD_DIR)/tool-userdb.o: user/tools/userdb.c user/tools/userdb.h user/tools/digest.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/tool-userdb_io.o: user/tools/userdb_io.c user/tools/userdb.h user/tools/common.h user/permissions_cli.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $@

$(LOGIN_ELFS): $(BUILD_DIR)/tool-%.elf: user/tools/%.c user/tools/userdb.h user/entry_security.h user/permissions_cli.h $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o $(BUILD_DIR)/tool-digest.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $(BUILD_DIR)/tool-$*.o
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-$*.o $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-digest.o $(BUILD_DIR)/tool-common.o -o $@

.PHONY: stage-user-database test-perm-db-host test-login
stage-user-database:
	@python3 scripts/stage_user_database.py $(BUILD_DIR)/initramfs/etc

test-perm-db-host:
	@python3 scripts/test_user_database_host.py

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
$(INITRAMFS_TAR): $(LOGIN_ELFS) stage-user-database scripts/stage_user_database.py

$(BUILD_DIR)/tool-common.o: user/tools/common.c user/tools/common.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(addprefix $(BUILD_DIR)/tool-,$(addsuffix .o,$(STREAM_TOOLS))): $(BUILD_DIR)/tool-%.o: user/tools/%.c user/tools/common.h user/permissions_cli.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(STREAM_TOOL_ELFS): $(BUILD_DIR)/tool-%.elf: $(BUILD_DIR)/tool-%.o user/tools/start.asm user/shell.ld $(BUILD_DIR)/tool-common.o
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-$*.o $(BUILD_DIR)/tool-common.o -o $@

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
SHELL_HEADERS := user/entry_security.h $(wildcard user/shell/*.h) src/include/terminal.h src/include/syscall_abi.h user/tools/userdb.h
SHELL_OBJECTS := $(patsubst user/shell/%.c,$(BUILD_DIR)/shell-%.o,$(SHELL_MODULES)) $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-digest.o

$(BUILD_DIR)/shell-%.o: user/shell/%.c $(SHELL_HEADERS)
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_SHELL_ELF): $(SHELL_OBJECTS) $(SHELL_HEADERS) $(USER_DIR)/shell.c $(USER_DIR)/shell_start.asm $(USER_DIR)/shell.ld src/fs/vfs.h src/include/types.h src/kernel/syscall.h src/arch/x86_64/idt.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $(USER_DIR)/shell.c -o $(BUILD_DIR)/shell.o
	@$(AS) -f elf64 $(USER_DIR)/shell_start.asm -o $(BUILD_DIR)/shell_start.o
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
$(INITRAMFS_TAR): $(USER_INIT_ELF) $(USER_HELLO_ELF) $(USER_DUAL_STREAM_ELF) $(USER_SHELL_ELF) $(USER_SH_BUILTIN_ELF) $(USER_PS_ELF) $(USER_SYSINFO_ELF) $(USER_DMESG_ELF) $(USER_IFCONFIG_ELF) $(USER_IFUP_ELF) $(USER_TOP_ELF) $(USER_NANO_ELF) $(USER_PING_ELF) $(USER_PING_PROBE_ELF) $(USER_UDP_ELFS) $(USER_TCP_ELF) $(USER_TCP_SERVER_ELF) $(USER_NC_ELF) $(USER_NSLOOKUP_ELF) $(USER_DNSPROBE_ELF) $(USER_TCPDEADLINE_ELF) $(USER_WGET_ELF) $(USER_DOWNLOAD_ELF) $(STREAM_TOOL_ELFS) $(USER_DISK_ELF) $(CHECKSUM_ELFS) $(USER_TRACEROUTE_ELF) $(USER_TAR_ELF) COMMANDS.md Makefile scripts/create_initramfs.py
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/etc $(BUILD_DIR)/initramfs/docs $(BIN_DIR)
	@cp -f $(USER_INIT_ELF) $(BUILD_DIR)/initramfs/bin/init
	@cp -f $(USER_SHELL_ELF) $(BUILD_DIR)/initramfs/bin/shell
	@$(foreach tool,$(LOGIN_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@cp -f $(USER_SH_BUILTIN_ELF) $(BUILD_DIR)/initramfs/bin/sh-builtin
	@cp -f $(USER_HELLO_ELF) $(BUILD_DIR)/initramfs/bin/hello
	@cp -f $(USER_DUAL_STREAM_ELF) $(BUILD_DIR)/initramfs/bin/dual_stream
	@cp -f $(USER_PS_ELF) $(BUILD_DIR)/initramfs/bin/ps
	@cp -f $(USER_SYSINFO_ELF) $(BUILD_DIR)/initramfs/bin/sysinfo
	@cp -f $(USER_DMESG_ELF) $(BUILD_DIR)/initramfs/bin/dmesg
	@cp -f $(USER_IFCONFIG_ELF) $(BUILD_DIR)/initramfs/bin/ifconfig
	@cp -f $(USER_IFUP_ELF) $(BUILD_DIR)/initramfs/bin/ifup
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
	@cp -f $(USER_DOWNLOAD_ELF) $(BUILD_DIR)/initramfs/bin/download
	@cp -f $(USER_TOP_ELF) $(BUILD_DIR)/initramfs/bin/top
	@cp -f $(USER_NANO_ELF) $(BUILD_DIR)/initramfs/bin/nano
	@$(foreach tool,$(STREAM_TOOLS) disk,cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@$(foreach tool,$(CHECKSUM_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@cp -f $(USER_TRACEROUTE_ELF) $(BUILD_DIR)/initramfs/bin/traceroute
	@cp -f $(USER_TAR_ELF) $(BUILD_DIR)/initramfs/bin/tar

	@printf "========================================================\n  Welcome to FortressOS (x86_64 SMP) — by Pride1922\n  \"Security through Isolation and Elegance\"\n========================================================\n" > $(BUILD_DIR)/initramfs/etc/motd
	@printf "FortressOS Documentation\nThe Ring 3 shell supports help, ls, view and echo.\nExternal cat preserves bytes; head, tail and wc process streams. Use TOOL --help.\nFull command reference available in /docs/commands.txt\n" > $(BUILD_DIR)/initramfs/docs/readme.txt
	@cp -f COMMANDS.md $(BUILD_DIR)/initramfs/docs/commands.txt
	@printf "# Fortress Network Configuration\naddress 10.0.2.15/24\ngateway 10.0.2.2\ndns 10.0.2.3\n" > $(BUILD_DIR)/initramfs/etc/network.conf
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

# Package bootable raw disk image (dual-boot GPT/ESP + journaled EXT4)
.PHONY: test-default-ext4-image
test-default-ext4-image: $(BOOTABLE_IMG)
	@python3 scripts/test_default_ext4_image.py --firmware bios --cpus 8 --output $(BUILD_DIR)/default-ext4-bios-$(shell date +%s)
	@python3 scripts/test_default_ext4_image.py --firmware uefi --cpus 8 --output $(BUILD_DIR)/default-ext4-uefi-$(shell date +%s)

img: $(BOOTABLE_IMG)

$(BOOTABLE_IMG): $(KERNEL_ELF) $(INITRAMFS_TAR) limine.conf scripts/create_boot_img.py scripts/initialize_ext4_journal.py limine-setup $(BOOTABLE_ISO)
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
NET_SOCKET_HOST_SRCS := tests/net_socket_host.c tests/net_lock_host.c src/net/net_socket.c src/net/net_socket_syscall.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c $(NET_TCP_STACK_SRCS)
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
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm -Isrc/fs $(subst tests/net_socket_host.c,tests/net_tcp_socket_host.c,$(NET_SOCKET_HOST_SRCS)) -Wl,--wrap=net_tcp_send -o $(BUILD_DIR)/net_tcp_socket_host
	@$(BUILD_DIR)/net_tcp_socket_host
	@python3 tests/test_tcp_deadline_guard.py
.PHONY: ext4-fixtures
ext4-fixtures:
	@python3 scripts/create_ext4_fixtures.py

.PHONY: test-ext4-format-host
test-ext4-format-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Itests/pipe_host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm tests/ext4_format_host.c -o $(BUILD_DIR)/ext4_format_host
	@python3 scripts/test_ext4_format_host.py

.PHONY: test-wget-host test-wget
test-wget-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include user/wget_codec.c tests/wget_host.c -o $(BUILD_DIR)/wget_host
	@$(BUILD_DIR)/wget_host

test-wget: $(BOOTABLE_ISO) $(NVME_GPT_IMG) test-wget-host
	@python3 scripts/test_wget.py

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
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_eth_host.c tests/net_lock_host.c src/net/net.c src/net/net_ping.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c src/net/arp.c -o $(BUILD_DIR)/net_eth_host
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
	@$(CC) -O1 -g -DTEST_SMP_MEMORY -pthread -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include -Isrc/net -Isrc/kernel -Isrc/drivers -Isrc/arch/x86_64 -Isrc/mm tests/net_ifconfig_host.c tests/net_lock_host.c src/net/net.c src/net/net_ping.c src/net/net_ipv4.c src/net/udp.c src/net/icmp.c src/net/checksum.c src/net/ipv4.c src/net/eth.c src/net/arp.c -o $(BUILD_DIR)/net_ifconfig_host
	@$(BUILD_DIR)/net_ifconfig_host

.PHONY: test-net-ifconfig-host test-net-ifconfig test-net-ifup
test-net-ifconfig: $(BOOTABLE_ISO) test-net-ifconfig-host
	@python3 scripts/test_net_ifconfig.py

test-net-ifup: $(BOOTABLE_ISO) test-net-ifconfig-host
	@python3 scripts/test_net_ifup.py

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

.PHONY: test-perm-spawn-host test-sudo
test-perm-spawn-host:
	@python3 scripts/test_perm_spawn_host.py
test-sudo:
	@python3 scripts/test_sudo.py

$(BUILD_DIR)/perm_phase4_user.elf: tests/perm_phase4_user.c user/entry_security.h user/tools/common.h user/permissions_cli.h $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -c $< -o $(BUILD_DIR)/perm_phase4_user.o
	@$(AS) -f elf64 -DTOOL_ENTRY=phase4_main user/tools/start.asm -o $(BUILD_DIR)/perm_phase4_user_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/perm_phase4_user_start.o $(BUILD_DIR)/perm_phase4_user.o $(BUILD_DIR)/tool-common.o -o $@

$(USER_SH_BUILTIN_ELF): user/entry_security.h
