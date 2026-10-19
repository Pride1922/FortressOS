$(INITRAMFS_TAR): scripts/replace.mk
ifeq ($(REPLACE_TEST_PROBE),1)
$(INITRAMFS_TAR): $(BUILD_DIR)/replace-probe.elf
endif
$(BUILD_DIR)/replace-probe.elf: tests/replace_probe.c tests/replace_probe.ld $(BUILD_DIR)/tool-common.o user/tools/common.h src/include/syscall_abi.h
	$(CC) $(CFLAGS) -Os -fno-pie -Iuser/tools -Wframe-larger-than=1024 -fstack-usage -c $< -o $(BUILD_DIR)/replace-probe.o
	$(AS) -f elf64 -DTOOL_ENTRY=replace_probe_main user/tools/start.asm -o $(BUILD_DIR)/replace-probe-start.o
	$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T tests/replace_probe.ld $(BUILD_DIR)/replace-probe-start.o $(BUILD_DIR)/replace-probe.o $(BUILD_DIR)/tool-common.o -o $@
.PHONY: test-ext4-replace
test-ext4-replace:
	python3 scripts/test_ext4_replace.py
