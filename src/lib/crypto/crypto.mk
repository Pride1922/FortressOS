# User-space only: no crypto syscalls or kernel copy of the vendor library.
CRYPTO_DIR := src/lib/crypto
CRYPTO_BUILD := $(BUILD_DIR)/crypto
CRYPTO_LIB := $(CRYPTO_BUILD)/libfortresscrypto.a
CRYPTO_API := crypto_wipe crypto_verify32 crypto_sha256_init crypto_sha256_update crypto_sha256_final crypto_sha256 crypto_sha512_init crypto_sha512_update crypto_sha512_final crypto_sha512 crypto_hmac_sha256 crypto_ed25519_from_seed crypto_ed25519_keygen crypto_ed25519_sign crypto_ed25519_verify crypto_x25519_from_seed crypto_x25519_keygen crypto_x25519 crypto_aead_lock crypto_aead_unlock
CRYPTO_UNITS := hash curve aead entropy
CRYPTO_OBJS := $(addprefix $(CRYPTO_BUILD)/,$(addsuffix .o,$(CRYPTO_UNITS))) $(CRYPTO_BUILD)/monocypher.o $(CRYPTO_BUILD)/ed25519.o $(CRYPTO_BUILD)/digest.o
CRYPTO_FLAGS := $(CFLAGS) -Os -fno-pie -fno-builtin -ffunction-sections -fdata-sections -fstack-usage -fno-asynchronous-unwind-tables -I$(CRYPTO_DIR)/upstream/src

all: $(CRYPTO_LIB)
$(CRYPTO_BUILD)/%.o: $(CRYPTO_DIR)/%.c src/include/crypto.h $(CRYPTO_DIR)/internal.h $(CRYPTO_DIR)/namespace.h $(CRYPTO_DIR)/namespace_end.h
	@mkdir -p $(CRYPTO_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -c $< -o $@
$(CRYPTO_BUILD)/monocypher.o: $(CRYPTO_DIR)/upstream/src/monocypher.c $(CRYPTO_DIR)/upstream/src/monocypher.h $(CRYPTO_DIR)/namespace.h
	@mkdir -p $(CRYPTO_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -include $(CRYPTO_DIR)/namespace.h -c $< -o $@
$(CRYPTO_BUILD)/ed25519.o: $(CRYPTO_DIR)/upstream/src/optional/monocypher-ed25519.c $(CRYPTO_DIR)/upstream/src/optional/monocypher-ed25519.h $(CRYPTO_DIR)/upstream/src/monocypher.h $(CRYPTO_DIR)/namespace.h
	@mkdir -p $(CRYPTO_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -include $(CRYPTO_DIR)/namespace.h -c $< -o $@
$(CRYPTO_BUILD)/digest.o: user/tools/digest.c user/tools/digest.h
	@mkdir -p $(CRYPTO_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -c $< -o $@
$(CRYPTO_BUILD)/runtime.o: src/lib/string.c src/include/string.h
	@mkdir -p $(CRYPTO_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -c $< -o $@
$(CRYPTO_LIB): $(CRYPTO_OBJS) $(CRYPTO_BUILD)/runtime.o
	@$(LD) -r --gc-sections $(addprefix -u ,$(CRYPTO_API)) $(CRYPTO_OBJS) -o $(CRYPTO_BUILD)/library.o
	@ar rcs $@ $(CRYPTO_BUILD)/library.o $(CRYPTO_BUILD)/runtime.o

.PHONY: test-crypto-host test-crypto
test-crypto-host:
	@python3 scripts/test_crypto_host.py
test-crypto:
	@python3 scripts/test_crypto.py

$(BUILD_DIR)/crypto-probe.elf: tests/crypto_probe.c tests/crypto_vectors.h $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CRYPTO_FLAGS) -Itests -Iuser/tools -c $< -o $(CRYPTO_BUILD)/probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=crypto_probe_main user/tools/start.asm -o $(CRYPTO_BUILD)/probe-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld $(CRYPTO_BUILD)/probe-start.o $(CRYPTO_BUILD)/probe.o $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o -o $@

-include $(wildcard $(CRYPTO_BUILD)/*.d)
