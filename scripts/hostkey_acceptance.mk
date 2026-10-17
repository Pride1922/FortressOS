# Packaged solely in an explicit isolated guest build. No release probe.
$(INITRAMFS_TAR): scripts/hostkey_acceptance.mk
ifeq ($(HOSTKEY_TEST_PROBE),1)
$(INITRAMFS_TAR): $(BUILD_DIR)/hostkey-probe.elf
endif
$(BUILD_DIR)/hostkey-probe.elf: tests/hostkey_probe.c tests/hostkey_probe.ld src/include/hostkey_abi.h user/permissions_cli.h $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CRYPTO_FLAGS) -Iuser/tools -c $< -o $(BUILD_DIR)/hostkey-probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=hostkey_probe_main user/tools/start.asm -o $(BUILD_DIR)/hostkey-probe-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T tests/hostkey_probe.ld $(BUILD_DIR)/hostkey-probe-start.o $(BUILD_DIR)/hostkey-probe.o $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o -o $@
.PHONY: test-ssh-hostkeys-acceptance
test-ssh-hostkeys-acceptance:
	@python3 scripts/test_ssh_hostkey_acceptance.py
