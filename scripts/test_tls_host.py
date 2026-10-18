#!/usr/bin/env python3
import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build", "tls")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
BIN_OUT = os.path.join(BUILD_DIR, "tls_unit_host")
LIB_OUT = os.path.join(BUILD_DIR, "libfortresstls.a")

CLIENT_SRCS = [
    'src/lib/tls/fortress_tls.c',
    'src/lib/tls/bearssl/src/settings.c',
    'src/lib/tls/bearssl/src/ssl/ssl_engine.c',
    'src/lib/tls/bearssl/src/ssl/ssl_client.c',
    'src/lib/tls/bearssl/src/ssl/ssl_hs_client.c',
    'src/lib/tls/bearssl/src/ssl/ssl_io.c',
    'src/lib/tls/bearssl/src/ssl/ssl_hashes.c',
    'src/lib/tls/bearssl/src/ssl/ssl_lru.c',
    'src/lib/tls/bearssl/src/ssl/ssl_rec_gcm.c',
    'src/lib/tls/bearssl/src/ssl/ssl_rec_chapol.c',
    'src/lib/tls/bearssl/src/ssl/prf.c',
    'src/lib/tls/bearssl/src/ssl/prf_sha256.c',
    'src/lib/tls/bearssl/src/ssl/prf_sha384.c',
    'src/lib/tls/bearssl/src/x509/x509_minimal.c',
    'src/lib/tls/bearssl/src/x509/x509_decoder.c',
    'src/lib/tls/bearssl/src/hash/sha2small.c',
    'src/lib/tls/bearssl/src/hash/sha2big.c',
    'src/lib/tls/bearssl/src/hash/dig_size.c',
    'src/lib/tls/bearssl/src/hash/dig_oid.c',
    'src/lib/tls/bearssl/src/hash/multihash.c',
    'src/lib/tls/bearssl/src/hash/mgf1.c',
    'src/lib/tls/bearssl/src/hash/ghash_ctmul.c',
    'src/lib/tls/bearssl/src/hash/ghash_ctmul32.c',
    'src/lib/tls/bearssl/src/ec/ec_all_m31.c',
    'src/lib/tls/bearssl/src/ec/ec_c25519_m31.c',
    'src/lib/tls/bearssl/src/ec/ec_p256_m31.c',
    'src/lib/tls/bearssl/src/ec/ec_prime_i31.c',
    'src/lib/tls/bearssl/src/ec/ec_curve25519.c',
    'src/lib/tls/bearssl/src/ec/ec_secp256r1.c',
    'src/lib/tls/bearssl/src/ec/ec_secp384r1.c',
    'src/lib/tls/bearssl/src/ec/ec_secp521r1.c',
    'src/lib/tls/bearssl/src/ec/ecdsa_i31_vrfy_asn1.c',
    'src/lib/tls/bearssl/src/ec/ecdsa_i31_vrfy_raw.c',
    'src/lib/tls/bearssl/src/ec/ecdsa_i31_bits.c',
    'src/lib/tls/bearssl/src/ec/ecdsa_atr.c',
    'src/lib/tls/bearssl/src/ec/ecdsa_default_vrfy_asn1.c',
    'src/lib/tls/bearssl/src/rsa/rsa_i31_pub.c',
    'src/lib/tls/bearssl/src/rsa/rsa_i31_pkcs1_vrfy.c',
    'src/lib/tls/bearssl/src/rsa/rsa_i62_pub.c',
    'src/lib/tls/bearssl/src/rsa/rsa_i62_pkcs1_vrfy.c',
    'src/lib/tls/bearssl/src/rsa/rsa_pkcs1_sig_unpad.c',
    'src/lib/tls/bearssl/src/rsa/rsa_default_pkcs1_vrfy.c',
    'src/lib/tls/bearssl/src/rsa/rsa_default_pub.c',
    'src/lib/tls/bearssl/src/symcipher/aes_common.c',
    'src/lib/tls/bearssl/src/symcipher/aes_ct.c',
    'src/lib/tls/bearssl/src/symcipher/aes_ct_enc.c',
    'src/lib/tls/bearssl/src/symcipher/aes_ct_ctr.c',
    'src/lib/tls/bearssl/src/symcipher/chacha20_ct.c',
    'src/lib/tls/bearssl/src/symcipher/poly1305_ctmul.c',
    'src/lib/tls/bearssl/src/symcipher/poly1305_ctmul32.c',
    'src/lib/tls/bearssl/src/symcipher/poly1305_ctmulq.c',
    'src/lib/tls/bearssl/src/aead/gcm.c',
    'src/lib/tls/bearssl/src/aead/ccm.c',
    'src/lib/tls/bearssl/src/int/i31_add.c',
    'src/lib/tls/bearssl/src/int/i31_bitlen.c',
    'src/lib/tls/bearssl/src/int/i31_decmod.c',
    'src/lib/tls/bearssl/src/int/i31_decode.c',
    'src/lib/tls/bearssl/src/int/i31_decred.c',
    'src/lib/tls/bearssl/src/int/i31_encode.c',
    'src/lib/tls/bearssl/src/int/i31_fmont.c',
    'src/lib/tls/bearssl/src/int/i31_iszero.c',
    'src/lib/tls/bearssl/src/int/i31_moddiv.c',
    'src/lib/tls/bearssl/src/int/i31_modpow.c',
    'src/lib/tls/bearssl/src/int/i31_modpow2.c',
    'src/lib/tls/bearssl/src/int/i31_montmul.c',
    'src/lib/tls/bearssl/src/int/i31_mulacc.c',
    'src/lib/tls/bearssl/src/int/i31_muladd.c',
    'src/lib/tls/bearssl/src/int/i31_ninv31.c',
    'src/lib/tls/bearssl/src/int/i31_reduce.c',
    'src/lib/tls/bearssl/src/int/i31_rshift.c',
    'src/lib/tls/bearssl/src/int/i31_sub.c',
    'src/lib/tls/bearssl/src/int/i31_tmont.c',
    'src/lib/tls/bearssl/src/int/i32_div32.c',
    'src/lib/tls/bearssl/src/int/i62_modpow2.c',
    'src/lib/tls/bearssl/src/mac/hmac.c',
    'src/lib/tls/bearssl/src/mac/hmac_ct.c',
    'src/lib/tls/bearssl/src/rand/hmac_drbg.c',
    'src/lib/tls/bearssl/src/codec/ccopy.c',
    'src/lib/tls/bearssl/src/codec/dec16be.c',
    'src/lib/tls/bearssl/src/codec/dec32be.c',
    'src/lib/tls/bearssl/src/codec/dec64be.c',
    'src/lib/tls/bearssl/src/codec/dec32le.c',
    'src/lib/tls/bearssl/src/codec/enc16be.c',
    'src/lib/tls/bearssl/src/codec/enc32be.c',
    'src/lib/tls/bearssl/src/codec/enc64be.c',
    'src/lib/tls/bearssl/src/codec/enc32le.c',
]

def main():
    import shutil
    shutil.rmtree(OBJ_DIR, ignore_errors=True)
    os.makedirs(OBJ_DIR, exist_ok=True)
    os.chdir(REPO_ROOT)
    srcs = [os.path.abspath(s) for s in CLIENT_SRCS]
    incs = [
        '-I' + os.path.abspath('src/include'),
        '-I' + os.path.abspath('src/lib/tls/bearssl/inc'),
        '-I' + os.path.abspath('src/lib/tls/bearssl/src')
    ]
    print(f"Total TLS client profile sources: {len(srcs)}")

    print("=== [1/5] Compiling BearSSL client profile objects under -Os -fstack-usage ===")
    compile_cmd = [
        "gcc",
        "-std=c99",
        "-Os",
        "-fno-builtin",
        "-ffunction-sections",
        "-fdata-sections",
        "-fstack-usage",
        "-DBR_USE_UNIX_TIME=0",
        "-DBR_USE_URANDOM=0",
        "-DTEST_HOST=1",
    ] + incs + ["-c"] + srcs

    res = subprocess.run(compile_cmd, cwd=OBJ_DIR)
    if res.returncode != 0:
        print("FAIL: Compilation of BearSSL library objects failed")
        sys.exit(1)

    print("=== [2/5] Creating static library libfortresstls.a ===")
    obj_files = [f for f in os.listdir(OBJ_DIR) if f.endswith(".o")]
    if os.path.exists(LIB_OUT):
        os.unlink(LIB_OUT)
    ar_cmd = ["ar", "rcs", LIB_OUT] + obj_files
    res = subprocess.run(ar_cmd, cwd=OBJ_DIR)
    if res.returncode != 0:
        print("FAIL: ar rcs failed")
        sys.exit(1)

    print("=== [3/5] Building TLS unit host test binary with ASan/UBSan ===")
    test_cmd = [
        "gcc",
        "-std=c99",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DTEST_HOST=1",
        "-Isrc/include",
        "-Isrc/lib/tls/bearssl/inc",
        "-Isrc/lib/tls/bearssl/src",
        "-Itests",
        "tests/tls_unit_host.c",
        LIB_OUT,
        "-o", BIN_OUT
    ]
    res = subprocess.run(test_cmd)
    if res.returncode != 0:
        print("FAIL: Compilation of tests/tls_unit_host.c failed")
        sys.exit(1)

    print("=== [4/5] Running TLS unit test suite ===")
    res = subprocess.run([BIN_OUT])
    if res.returncode != 0:
        print("FAIL: tls_unit_host tests failed")
        sys.exit(1)

    print("=== [5/5] Auditing Stack Usage (-fstack-usage) & Footprint (Gate G1) ===")
    max_frame = 0
    max_func = ""
    violations = []
    rsa_exception_found = False

    for fname in os.listdir(OBJ_DIR):
        if fname.endswith(".su"):
            su_path = os.path.join(OBJ_DIR, fname)
            with open(su_path, "r") as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) >= 3:
                        loc, size, qual = parts[0], int(parts[1]), parts[2]
                        if size > max_frame:
                            max_frame = size
                            max_func = f"{loc} ({size} B)"

                        # Check exception vs invariant
                        if "rsa_i31_public" in loc or "rsa_i62_public" in loc:
                            rsa_exception_found = True
                            if size > 2400:
                                violations.append(f"RSA exception exceeded: {loc} = {size} B > 2400 B")
                        elif "ec_prime_i31" in loc:
                            # ec_prime_i31 generic prime curve operations allowed up to 1280 B ceiling
                            if size > 1280:
                                violations.append(f"Generic curve exception exceeded: {loc} = {size} B > 1280 B")
                        else:
                            if size > 1024:
                                violations.append(f"Stack invariant violation: {loc} = {size} B > 1024 B")

    print(f"    -> Maximum measured stack frame in library: {max_func}")
    assert rsa_exception_found, "br_rsa_i31_public stack frame not found"
    if violations:
        print("FAIL: Stack ceiling violations detected:")
        for v in violations:
            print("  ", v)
        sys.exit(1)
    print("    -> Stack frame invariant validated: all client profile frames <= 1024 B (RSA exception <= 2400 B, generic curve <= 1280 B).")

    # Measure code and bss footprint of reachable client profile via consumer binary
    consumer_c = os.path.join(OBJ_DIR, "consumer.c")
    consumer_elf = os.path.join(OBJ_DIR, "consumer.elf")
    with open(consumer_c, "w") as f:
        f.write(
            '#include "tls.h"\n'
            'int main(void) {\n'
            '    fortress_tls_t ctx;\n'
            '    fortress_tls_client_init(&ctx, (const br_x509_trust_anchor *)0, 0);\n'
            '    fortress_tls_connect(&ctx, 0, "example.com", 0);\n'
            '    fortress_tls_read(&ctx, (void *)0, 0, 0);\n'
            '    fortress_tls_write(&ctx, (const void *)0, 0, 0);\n'
            '    fortress_tls_flush(&ctx, 0);\n'
            '    fortress_tls_close(&ctx, 0);\n'
            '    return 0;\n'
            '}\n'
        )
    consumer_cmd = [
        "gcc", "-std=c99", "-Os", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections",
        "-Isrc/include", "-Isrc/lib/tls/bearssl/inc", consumer_c, LIB_OUT,
        "-o", consumer_elf
    ]
    res = subprocess.run(consumer_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAIL: consumer build failed:\n{res.stderr}")
        sys.exit(1)

    size_res = subprocess.run(["size", consumer_elf], capture_output=True, text=True, check=True)
    total_text = 0
    total_data = 0
    total_bss = 0
    for line in size_res.stdout.strip().split("\n")[1:]:
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            total_text += int(p[0])
            total_data += int(p[1])
            total_bss += int(p[2])

    print(f"    -> Measured Reachable Client Profile Footprint: text={total_text} B (~{total_text//1024} KiB), data={total_data} B, bss={total_bss} B")
    assert total_text < 140000, f"Code footprint exceeded: {total_text} > 140 KiB (target ~118 KiB)"
    print("=== Phase TLS-1 Verification PASSED (Gate G1 & G2) ===")

if __name__ == "__main__":
    main()
