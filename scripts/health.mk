$(INITRAMFS_TAR): scripts/health.mk
ifeq ($(HEALTH_TEST_PROBE),1)
$(BUILD_DIR)/fs/ext4.o: CFLAGS += -DFORTRESS_EXT4_HEALTH_TEST
$(INITRAMFS_TAR): $(BUILD_DIR)/health-probe.elf
endif
$(BUILD_DIR)/health-probe.elf: tests/health_probe.c tests/replace_probe.ld $(BUILD_DIR)/tool-common.o user/tools/common.h src/include/syscall_abi.h
	$(CC) $(CFLAGS) -Os -fno-pie -Iuser/tools -Wframe-larger-than=1024 -fstack-usage -c $< -o $(BUILD_DIR)/health-probe.o
	$(AS) -f elf64 -DTOOL_ENTRY=health_probe_main user/tools/start.asm -o $(BUILD_DIR)/health-probe-start.o
	$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T tests/replace_probe.ld $(BUILD_DIR)/health-probe-start.o $(BUILD_DIR)/health-probe.o $(BUILD_DIR)/tool-common.o -o $@
.PHONY: test-ext4-health
test-ext4-health:
	python3 scripts/test_ext4_health.py
