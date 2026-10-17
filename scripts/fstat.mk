$(INITRAMFS_TAR): scripts/fstat.mk
# The diagnostic ELF is only packaged in explicitly isolated test builds.
ifeq ($(FSTAT_TEST_PROBE),1)
$(INITRAMFS_TAR): $(BUILD_DIR)/fstat-probe.elf
endif
$(BUILD_DIR)/fstat-probe.elf: tests/fstat_probe.c $(BUILD_DIR)/tool-common.o user/tools/common.h user/permissions_cli.h src/include/syscall_abi.h src/fs/vfs.h user/tools/start.asm user/shell.ld
	@$(CC) $(CFLAGS) -Os -fno-pie -Iuser/tools -fstack-usage -c $< -o $(BUILD_DIR)/fstat-probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=fstat_probe_main user/tools/start.asm -o $(BUILD_DIR)/fstat-probe-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T user/shell.ld $(BUILD_DIR)/fstat-probe-start.o $(BUILD_DIR)/fstat-probe.o $(BUILD_DIR)/tool-common.o -o $@
.PHONY: test-fstat-host test-fstat
test-fstat-host:
	@mkdir -p build
	@gcc -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -DFORTRESS_DAC_ENFORCED -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm -Isrc/kernel -Isrc/arch/x86_64 tests/fstat_host.c src/kernel/creds.c src/fs/permission_values.c -o build/fstat_host
	@build/fstat_host
test-fstat:
	@python3 scripts/test_fstat.py
