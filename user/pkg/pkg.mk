# Normal /bin/pkg: repository configuration and package operations.
PKG_UNITS := format manifest verify catalog cli transaction_cli mutation_cli system_cli repository fetch
PKG_BUILD := $(BUILD_DIR)/pkg
PKG_OBJS := $(addprefix $(PKG_BUILD)/,$(addsuffix .o,$(PKG_UNITS)))
USER_PKG_ELF := $(PKG_BUILD)/pkg.elf
PKG_FLAGS := $(CFLAGS) -Os -fno-pie -fno-builtin -ffunction-sections -fdata-sections -fstack-usage -Wframe-larger-than=1024 -DFORTRESS_PKG_FETCH=1

$(PKG_BUILD)/%.o: user/pkg/%.c
	@mkdir -p $(PKG_BUILD)
	@$(CC) $(PKG_FLAGS) -c $< -o $@

$(USER_PKG_ELF): $(PKG_OBJS) $(CRYPTO_LIB) $(BUILD_DIR)/tool-gzip.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=pkg_system_main user/tools/start.asm -o $(PKG_BUILD)/start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld $(PKG_BUILD)/start.o $(PKG_OBJS) $(BUILD_DIR)/tool-gzip.o $(BUILD_DIR)/tool-common.o $(CRYPTO_LIB) -o $@

-include $(PKG_OBJS:.o=.d)
