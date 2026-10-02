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
SMP     ?= 1

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
           -Isrc/fs \
           -Isrc/net

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

test-smp-memory-boot: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_memory_boot.py

.PHONY: test-smp-vmm
test-smp-vmm: $(BOOTABLE_ISO)
	@python3 scripts/test_smp_vmm.py

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
USER_PING_ELF := $(BUILD_DIR)/ping.elf
USER_PING_PROBE_ELF := $(BUILD_DIR)/net_ping_probe.elf
USER_UDP_ELFS := $(BUILD_DIR)/udptest.elf $(BUILD_DIR)/net_udp_probe.elf
USER_TCP_ELF := $(BUILD_DIR)/tcptest.elf
USER_TCP_SERVER_ELF := $(BUILD_DIR)/tcpserve.elf
USER_NC_ELF := $(BUILD_DIR)/nc.elf
USER_NSLOOKUP_ELF := $(BUILD_DIR)/nslookup.elf
USER_DNSPROBE_ELF := $(BUILD_DIR)/dnsprobe.elf
USER_TCPDEADLINE_ELF := $(BUILD_DIR)/tcpdeadline.elf
DNS_OBJECTS := $(BUILD_DIR)/dns.o $(BUILD_DIR)/dns_codec.o
$(DNS_OBJECTS): $(BUILD_DIR)/%.o: user/%.c user/dns.h user/dns_codec.h src/include/syscall_abi.h src/include/socket_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@
$(BUILD_DIR)/nslookup.o: user/nslookup.c user/dns.h user/dns_codec.h user/udp_common.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fstack-usage -c $< -o $@
$(USER_NSLOOKUP_ELF): $(BUILD_DIR)/nslookup.o $(DNS_OBJECTS) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=nslookup_main user/tools/start.asm -o $(BUILD_DIR)/nslookup_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/nslookup_start.o $(BUILD_DIR)/nslookup.o $(DNS_OBJECTS) -o $@
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
$(BUILD_DIR)/nc.o: user/nc.c user/dns.h user/udp_common.h src/include/socket_abi.h src/include/syscall_abi.h src/include/terminal.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -c $< -o $@
$(USER_NC_ELF): $(BUILD_DIR)/nc.o $(DNS_OBJECTS) user/tools/start.asm user/shell.ld
	@$(AS) $(ASFLAGS) -DTOOL_ENTRY=nc_main user/tools/start.asm -o $(BUILD_DIR)/nc_start.o
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/shell.ld $(BUILD_DIR)/nc_start.o $(BUILD_DIR)/nc.o $(DNS_OBJECTS) -o $@
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
STREAM_TOOLS := cat head tail wc
STREAM_TOOL_ELFS := $(addprefix $(BUILD_DIR)/tool-,$(addsuffix .elf,$(STREAM_TOOLS)))
INITRAMFS_TAR := $(BIN_DIR)/initramfs.tar

$(BUILD_DIR)/tool-common.o: user/tools/common.c user/tools/common.h src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(STREAM_TOOL_ELFS): $(BUILD_DIR)/tool-%.elf: user/tools/%.c user/tools/common.h user/tools/start.asm user/shell.ld $(BUILD_DIR)/tool-common.o src/include/types.h src/include/syscall_abi.h src/fs/vfs.h
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $(BUILD_DIR)/tool-$*.o
	@$(AS) -f elf64 -DTOOL_ENTRY=$*_main user/tools/start.asm -o $(BUILD_DIR)/tool-$*-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/tool-$*-start.o $(BUILD_DIR)/tool-$*.o $(BUILD_DIR)/tool-common.o -o $@

.PHONY: test-stream-tools-host
test-stream-tools-host:
	@python3 scripts/test_stream_tools_host.py

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
SHELL_HEADERS := $(wildcard user/shell/*.h) src/include/terminal.h src/include/syscall_abi.h
SHELL_OBJECTS := $(patsubst user/shell/%.c,$(BUILD_DIR)/shell-%.o,$(SHELL_MODULES))

$(BUILD_DIR)/shell-%.o: user/shell/%.c $(SHELL_HEADERS)
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(USER_SHELL_ELF): $(SHELL_OBJECTS) $(SHELL_HEADERS) $(USER_DIR)/shell.c $(USER_DIR)/shell_start.asm $(USER_DIR)/shell.ld src/fs/vfs.h src/include/types.h src/kernel/syscall.h src/arch/x86_64/idt.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $(USER_DIR)/shell.c -o $(BUILD_DIR)/shell.o
	@$(AS) -f elf64 $(USER_DIR)/shell_start.asm -o $(BUILD_DIR)/shell_start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/shell_start.o $(BUILD_DIR)/shell.o $(SHELL_OBJECTS) -o $@

# Runner ELF for /bin/sh-builtin: links builtin_exec, builtins, io (no UI/history/alias/editor).
SH_BUILTIN_OBJECTS := $(BUILD_DIR)/shell-builtin_exec.o $(BUILD_DIR)/shell-builtins.o $(BUILD_DIR)/shell-io.o
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
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/ping.o: $(USER_DIR)/ping.c src/include/types.h src/include/syscall_abi.h src/include/ping_abi.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -Wframe-larger-than=512 -fstack-usage -c $< -o $@

$(BUILD_DIR)/ping_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=ping_main $< -o $@

$(USER_PING_ELF): $(BUILD_DIR)/ping_start.o $(BUILD_DIR)/ping.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/ping_start.o $(BUILD_DIR)/ping.o -o $@

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

$(BUILD_DIR)/top.o: $(USER_DIR)/top.c src/include/types.h src/include/syscall_abi.h src/include/terminal.h
	@mkdir -p $(BUILD_DIR)
	@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -c $< -o $@

$(BUILD_DIR)/top_start.o: $(USER_DIR)/tools/start.asm
	@mkdir -p $(BUILD_DIR)
	@$(AS) -f elf64 -DTOOL_ENTRY=top_main $< -o $@

$(USER_TOP_ELF): $(BUILD_DIR)/top_start.o $(BUILD_DIR)/top.o $(USER_DIR)/shell.ld
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T $(USER_DIR)/shell.ld $(BUILD_DIR)/top_start.o $(BUILD_DIR)/top.o -o $@

# Build USTAR Initramfs archive
$(INITRAMFS_TAR): $(USER_INIT_ELF) $(USER_HELLO_ELF) $(USER_DUAL_STREAM_ELF) $(USER_SHELL_ELF) $(USER_SH_BUILTIN_ELF) $(USER_PS_ELF) $(USER_SYSINFO_ELF) $(USER_TOP_ELF) $(USER_PING_ELF) $(USER_PING_PROBE_ELF) $(USER_UDP_ELFS) $(USER_TCP_ELF) $(USER_TCP_SERVER_ELF) $(USER_NC_ELF) $(USER_NSLOOKUP_ELF) $(USER_DNSPROBE_ELF) $(USER_TCPDEADLINE_ELF) $(STREAM_TOOL_ELFS) Makefile
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/etc $(BUILD_DIR)/initramfs/docs $(BIN_DIR)
	@cp -f $(USER_INIT_ELF) $(BUILD_DIR)/initramfs/bin/init
	@cp -f $(USER_SHELL_ELF) $(BUILD_DIR)/initramfs/bin/shell
	@cp -f $(USER_SH_BUILTIN_ELF) $(BUILD_DIR)/initramfs/bin/sh-builtin
	@cp -f $(USER_HELLO_ELF) $(BUILD_DIR)/initramfs/bin/hello
	@cp -f $(USER_DUAL_STREAM_ELF) $(BUILD_DIR)/initramfs/bin/dual_stream
	@cp -f $(USER_PS_ELF) $(BUILD_DIR)/initramfs/bin/ps
	@cp -f $(USER_SYSINFO_ELF) $(BUILD_DIR)/initramfs/bin/sysinfo
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
	@cp -f $(USER_TOP_ELF) $(BUILD_DIR)/initramfs/bin/top
	@$(foreach tool,$(STREAM_TOOLS),cp -f $(BUILD_DIR)/tool-$(tool).elf $(BUILD_DIR)/initramfs/bin/$(tool);)
	@printf "========================================================\n  Welcome to FortressOS (x86_64 SMP) — by Pride1922\n  \"Security through Isolation and Elegance\"\n========================================================\n" > $(BUILD_DIR)/initramfs/etc/motd
	@printf "FortressOS Documentation\nThe Ring 3 shell supports help, ls, view and echo.\nExternal cat preserves bytes; head, tail and wc process streams. Use TOOL --help.\n" > $(BUILD_DIR)/initramfs/docs/readme.txt
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

test-net-nc-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/net_nc_host.c user/dns.c user/dns_codec.c -o $(BUILD_DIR)/net_nc_host
	@$(BUILD_DIR)/net_nc_host
test-net-tcp-matrix: $(BOOTABLE_ISO) test-net-tcp-fixture test-net-nc-host test-net-tcp-socket-host
	@python3 scripts/test_net_tcp_matrix.py --all
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
.PHONY: test-net-udp-host test-net-socket-host
.PHONY: test-net-dns-host
test-net-dns-host:
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -Isrc/include tests/net_dns_host.c user/dns_codec.c -o $(BUILD_DIR)/net_dns_host
	@$(BUILD_DIR)/net_dns_host
	@python3 tests/test_dns_fixture.py
	@python3 tests/test_dns_lan_peer.py
.PHONY: test-net-dns test-net-dns-lifecycle
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
