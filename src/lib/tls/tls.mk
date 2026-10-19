# User-space TLS client library based on BearSSL 0.6
TLS_DIR := src/lib/tls
TLS_BUILD := $(BUILD_DIR)/tls
TLS_LIB := $(TLS_BUILD)/libfortresstls.a

TLS_API := fortress_tls_client_init fortress_tls_connect fortress_tls_read fortress_tls_write fortress_tls_flush fortress_tls_close fortress_tls_strerror tls_clock_hook tls_rand_hook tls_send_hook tls_recv_hook fortress_tls_set_syscall_hook fortress_tls_load_anchors fortress_tls_parse_anchors

TLS_INCS := -Isrc/include -I$(TLS_DIR)/bearssl/inc -I$(TLS_DIR)/bearssl/src
TLS_FLAGS := $(CFLAGS) -D__FORTRESS__ -DBR_USE_UNIX_TIME=0 -DBR_USE_URANDOM=0 -Os -fno-pie -fno-builtin -ffunction-sections -fdata-sections -fstack-usage -fno-asynchronous-unwind-tables $(TLS_INCS)

TLS_SRCS := \
	$(TLS_DIR)/fortress_tls.c \
	$(TLS_DIR)/anchor_parser.c \
	$(TLS_DIR)/bearssl/src/settings.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_engine.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_client.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_hs_client.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_io.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_hashes.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_lru.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_rec_gcm.c \
	$(TLS_DIR)/bearssl/src/ssl/ssl_rec_chapol.c \
	$(TLS_DIR)/bearssl/src/ssl/prf.c \
	$(TLS_DIR)/bearssl/src/ssl/prf_sha256.c \
	$(TLS_DIR)/bearssl/src/ssl/prf_sha384.c \
	$(TLS_DIR)/bearssl/src/x509/x509_minimal.c \
	$(TLS_DIR)/bearssl/src/x509/x509_decoder.c \
	$(TLS_DIR)/bearssl/src/hash/sha2small.c \
	$(TLS_DIR)/bearssl/src/hash/sha2big.c \
	$(TLS_DIR)/bearssl/src/hash/dig_size.c \
	$(TLS_DIR)/bearssl/src/hash/dig_oid.c \
	$(TLS_DIR)/bearssl/src/hash/multihash.c \
	$(TLS_DIR)/bearssl/src/hash/mgf1.c \
	$(TLS_DIR)/bearssl/src/hash/ghash_ctmul.c \
	$(TLS_DIR)/bearssl/src/hash/ghash_ctmul32.c \
	$(TLS_DIR)/bearssl/src/ec/ec_all_m31.c \
	$(TLS_DIR)/bearssl/src/ec/ec_c25519_m31.c \
	$(TLS_DIR)/bearssl/src/ec/ec_p256_m31.c \
	$(TLS_DIR)/bearssl/src/ec/ec_prime_i31.c \
	$(TLS_DIR)/bearssl/src/ec/ec_curve25519.c \
	$(TLS_DIR)/bearssl/src/ec/ec_secp256r1.c \
	$(TLS_DIR)/bearssl/src/ec/ec_secp384r1.c \
	$(TLS_DIR)/bearssl/src/ec/ec_secp521r1.c \
	$(TLS_DIR)/bearssl/src/ec/ecdsa_i31_vrfy_asn1.c \
	$(TLS_DIR)/bearssl/src/ec/ecdsa_i31_vrfy_raw.c \
	$(TLS_DIR)/bearssl/src/ec/ecdsa_i31_bits.c \
	$(TLS_DIR)/bearssl/src/ec/ecdsa_atr.c \
	$(TLS_DIR)/bearssl/src/ec/ecdsa_default_vrfy_asn1.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_i31_pub.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_i31_pkcs1_vrfy.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_i62_pub.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_i62_pkcs1_vrfy.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_pkcs1_sig_unpad.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_default_pkcs1_vrfy.c \
	$(TLS_DIR)/bearssl/src/rsa/rsa_default_pub.c \
	$(TLS_DIR)/bearssl/src/symcipher/aes_common.c \
	$(TLS_DIR)/bearssl/src/symcipher/aes_ct.c \
	$(TLS_DIR)/bearssl/src/symcipher/aes_ct_enc.c \
	$(TLS_DIR)/bearssl/src/symcipher/aes_ct_ctr.c \
	$(TLS_DIR)/bearssl/src/symcipher/chacha20_ct.c \
	$(TLS_DIR)/bearssl/src/symcipher/poly1305_ctmul.c \
	$(TLS_DIR)/bearssl/src/symcipher/poly1305_ctmul32.c \
	$(TLS_DIR)/bearssl/src/symcipher/poly1305_ctmulq.c \
	$(TLS_DIR)/bearssl/src/aead/gcm.c \
	$(TLS_DIR)/bearssl/src/aead/ccm.c \
	$(TLS_DIR)/bearssl/src/int/i31_add.c \
	$(TLS_DIR)/bearssl/src/int/i31_bitlen.c \
	$(TLS_DIR)/bearssl/src/int/i31_decmod.c \
	$(TLS_DIR)/bearssl/src/int/i31_decode.c \
	$(TLS_DIR)/bearssl/src/int/i31_decred.c \
	$(TLS_DIR)/bearssl/src/int/i31_encode.c \
	$(TLS_DIR)/bearssl/src/int/i31_fmont.c \
	$(TLS_DIR)/bearssl/src/int/i31_iszero.c \
	$(TLS_DIR)/bearssl/src/int/i31_moddiv.c \
	$(TLS_DIR)/bearssl/src/int/i31_modpow.c \
	$(TLS_DIR)/bearssl/src/int/i31_modpow2.c \
	$(TLS_DIR)/bearssl/src/int/i31_montmul.c \
	$(TLS_DIR)/bearssl/src/int/i31_mulacc.c \
	$(TLS_DIR)/bearssl/src/int/i31_muladd.c \
	$(TLS_DIR)/bearssl/src/int/i31_ninv31.c \
	$(TLS_DIR)/bearssl/src/int/i31_reduce.c \
	$(TLS_DIR)/bearssl/src/int/i31_rshift.c \
	$(TLS_DIR)/bearssl/src/int/i31_sub.c \
	$(TLS_DIR)/bearssl/src/int/i31_tmont.c \
	$(TLS_DIR)/bearssl/src/int/i32_div32.c \
	$(TLS_DIR)/bearssl/src/int/i62_modpow2.c \
	$(TLS_DIR)/bearssl/src/mac/hmac.c \
	$(TLS_DIR)/bearssl/src/mac/hmac_ct.c \
	$(TLS_DIR)/bearssl/src/rand/hmac_drbg.c \
	$(TLS_DIR)/bearssl/src/codec/ccopy.c \
	$(TLS_DIR)/bearssl/src/codec/dec16be.c \
	$(TLS_DIR)/bearssl/src/codec/dec32be.c \
	$(TLS_DIR)/bearssl/src/codec/dec64be.c \
	$(TLS_DIR)/bearssl/src/codec/dec32le.c \
	$(TLS_DIR)/bearssl/src/codec/enc16be.c \
	$(TLS_DIR)/bearssl/src/codec/enc32be.c \
	$(TLS_DIR)/bearssl/src/codec/enc64be.c \
	$(TLS_DIR)/bearssl/src/codec/enc32le.c

TLS_OBJS := $(patsubst $(TLS_DIR)/%.c,$(TLS_BUILD)/obj/%.o,$(TLS_SRCS)) $(TLS_BUILD)/obj/string.o

$(TLS_BUILD)/obj/string.o: src/lib/string.c
	@mkdir -p $(dir $@)
	@$(CC) $(TLS_FLAGS) -c $< -o $@

$(TLS_BUILD)/obj/%.o: $(TLS_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(TLS_FLAGS) -c $< -o $@

$(TLS_LIB): $(TLS_OBJS)
	@mkdir -p $(TLS_BUILD)
	@rm -f $@
	@ar rcs $@ $(TLS_OBJS)

.PHONY: test-tls-unit-host
test-tls-unit-host:
	@python3 scripts/test_tls_host.py

.PHONY: test-tls-anchor-host
test-tls-anchor-host:
	@python3 scripts/test_tls_anchor_host.py

.PHONY: test-tls-fixture-host
test-tls-fixture-host:
	@python3 scripts/test_tls_fixture.py

.PHONY: test-tls-wget-host
test-tls-wget-host: test-tls-unit-host
	@python3 scripts/test_wget_https.py

.PHONY: test-tls-host
test-tls-host: test-tls-unit-host test-tls-fixture-host test-tls-wget-host

.PHONY: test-tls-all
test-tls-all: test-tls-unit-host test-tls-anchor-host test-tls-fixture-host test-tls-wget-host

.PHONY: test-tls-qemu
test-tls-qemu: $(BOOTABLE_ISO)
	@python3 scripts/test_tls_qemu.py

