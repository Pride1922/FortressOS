# User-space only: no crypto syscalls or kernel copy of the vendor library.
CRYPTO_DIR := src/lib/crypto
CRYPTO_BUILD := $(BUILD_DIR)/crypto
CRYPTO_LIB := $(CRYPTO_BUILD)/libfortresscrypto.a
CRYPTO_API := crypto_wipe crypto_verify32 crypto_sha256_init crypto_sha256_update crypto_sha256_final crypto_sha256 crypto_sha512_init crypto_sha512_update crypto_sha512_final crypto_sha512 crypto_hmac_sha256 crypto_ed25519_from_seed crypto_ed25519_keygen crypto_ed25519_sign crypto_ed25519_verify crypto_x25519_from_seed crypto_x25519_keygen crypto_x25519 crypto_aead_lock crypto_aead_unlock
CRYPTO_API += crypto_hkdf_extract crypto_hkdf_expand crypto_hkdf
CRYPTO_UNITS := hash curve aead entropy
CRYPTO_API += ssh_hostkey_encode ssh_hostkey_decode ssh_hostkey_generate ssh_hostkey_public ssh_hostkey_fingerprint
CRYPTO_UNITS += ssh_hostkey
CRYPTO_OBJS := $(addprefix $(CRYPTO_BUILD)/,$(addsuffix .o,$(CRYPTO_UNITS))) $(CRYPTO_BUILD)/monocypher.o $(CRYPTO_BUILD)/ed25519.o $(CRYPTO_BUILD)/digest.o
CRYPTO_FLAGS := $(CFLAGS) -D__FORTRESS__ -Os -fno-pie -fno-builtin -ffunction-sections -fdata-sections -fstack-usage -fno-asynchronous-unwind-tables -I$(CRYPTO_DIR)/upstream/src

all: $(CRYPTO_LIB)
all: $(BUILD_DIR)/ssh-keygen.elf
all: $(BUILD_DIR)/sshd.elf
$(BUILD_DIR)/ssh-keygen.elf: user/tools/ssh_keygen.c src/include/ssh_hostkey.h src/include/hostkey_abi.h $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CRYPTO_FLAGS) -Iuser/tools -c $< -o $(CRYPTO_BUILD)/ssh-keygen.o
	@$(AS) -f elf64 -DTOOL_ENTRY=ssh_keygen_main user/tools/start.asm -o $(CRYPTO_BUILD)/ssh-keygen-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld $(CRYPTO_BUILD)/ssh-keygen-start.o $(CRYPTO_BUILD)/ssh-keygen.o $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o -o $@

SSHD_DIR := user/sshd
SSHD_BUILD := $(BUILD_DIR)/sshd
SSHD_SRCS := $(wildcard $(SSHD_DIR)/*.c)
SSHD_OBJS := $(patsubst $(SSHD_DIR)/%.c,$(SSHD_BUILD)/%.o,$(SSHD_SRCS))

$(SSHD_BUILD)/%.o: $(SSHD_DIR)/%.c
	@mkdir -p $(SSHD_BUILD)
	@$(CC) $(CRYPTO_FLAGS) -Iuser/sshd -Iuser/tools -c $< -o $@

$(BUILD_DIR)/sshd.elf: $(SSHD_OBJS) $(CRYPTO_LIB) $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(AS) -f elf64 -DTOOL_ENTRY=sshd_main user/tools/start.asm -o $(SSHD_BUILD)/sshd-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld $(SSHD_BUILD)/sshd-start.o $(SSHD_OBJS) $(CRYPTO_LIB) $(BUILD_DIR)/tool-userdb.o $(BUILD_DIR)/tool-userdb_io.o $(BUILD_DIR)/tool-common.o -o $@
$(CRYPTO_BUILD)/%.o: $(CRYPTO_DIR)/%.c src/include/crypto.h src/include/ssh_hostkey.h $(CRYPTO_DIR)/internal.h $(CRYPTO_DIR)/namespace.h $(CRYPTO_DIR)/namespace_end.h
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

.PHONY: test-crypto-host test-crypto test-crypto-hkdf-host
test-crypto-host:
	@python3 scripts/test_crypto_host.py

test-crypto-hkdf-host:
	@python3 scripts/test_crypto_hkdf_host.py

.PHONY: test-ssh-hostkeys-host
test-ssh-hostkeys-host:
	@python3 scripts/test_ssh_hostkeys_host.py

.PHONY: test-ssh-transport-host
test-ssh-transport-host:
	@python3 scripts/test_ssh_transport_host.py

.PHONY: test-ssh-auth-host
test-ssh-auth-host:
	@python3 scripts/test_ssh_auth_host.py

.PHONY: test-ssh-channel-host
test-ssh-channel-host:
	@python3 scripts/test_ssh_channel_host.py

.PHONY: test-ssh-pty-host
test-ssh-pty-host:
	@python3 scripts/test_ssh_pty_host.py

.PHONY: test-ssh-daemon-host
test-ssh-daemon-host:
	@python3 scripts/test_ssh_daemon_host.py

.PHONY: test-ssh-hostkeys-storage-host
test-ssh-hostkeys-storage-host: test-ssh-hostkeys-host
	@python3 scripts/test_ssh_hostkey_storage.py

.PHONY: test-ssh-hostkeys-installer-host
test-ssh-hostkeys-installer-host: all test-ssh-hostkeys-host
	@python3 scripts/test_ssh_installer_host.py

.PHONY: test-ssh-hostkeys
test-ssh-hostkeys: all
	@python3 scripts/test_ssh_hostkeys.py

.PHONY: test-ssh
test-ssh: $(BOOTABLE_ISO) $(NVME_GPT_IMG)
	@python3 scripts/test_ssh.py

test-crypto:
	@python3 scripts/test_crypto.py

$(BUILD_DIR)/crypto-probe.elf: tests/crypto_probe.c tests/crypto_vectors.h $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o user/tools/start.asm user/shell.ld
	@$(CC) $(CRYPTO_FLAGS) -Itests -Iuser/tools -c $< -o $(CRYPTO_BUILD)/probe.o
	@$(AS) -f elf64 -DTOOL_ENTRY=crypto_probe_main user/tools/start.asm -o $(CRYPTO_BUILD)/probe-start.o
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld $(CRYPTO_BUILD)/probe-start.o $(CRYPTO_BUILD)/probe.o $(CRYPTO_LIB) $(BUILD_DIR)/tool-common.o -o $@

-include $(wildcard $(CRYPTO_BUILD)/*.d)
