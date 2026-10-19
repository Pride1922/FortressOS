#!/usr/bin/env python3
"""
scripts/test_tls_anchor_host.py
Verification runner for Phase TLS-2: Trust Store & Anchor Compiler.
Compiles and executes tests/tls_anchor_host.c under AddressSanitizer and UndefinedBehaviorSanitizer (ASan/UBSan),
audits stack usage under -fstack-usage, and validates parsing against Mozilla CA bundle.
"""

import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build", "tls")
OBJ_DIR = os.path.join(BUILD_DIR, "anchor_obj")
BIN_OUT = os.path.join(BUILD_DIR, "tls_anchor_host")
LIB_OUT = os.path.join(BUILD_DIR, "libfortresstls_anchor.a")
CERT_DIR = os.path.join(REPO_ROOT, "build", "ssl", "certs")
ANCHOR_FILE = os.path.join(CERT_DIR, "anchors.v1")

CLIENT_SRCS = [
    'src/lib/tls/fortress_tls.c',
    'src/lib/tls/anchor_parser.c',
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
    shutil.rmtree(OBJ_DIR, ignore_errors=True)
    os.makedirs(OBJ_DIR, exist_ok=True)
    os.chdir(REPO_ROOT)

    print("=== [1/5] Building trust anchor bundle if needed ===")
    if not os.path.exists(ANCHOR_FILE):
        os.makedirs(CERT_DIR, exist_ok=True)
        host_bundle = "/etc/ssl/certs/ca-certificates.crt"
        if not os.path.exists(host_bundle):
            print(f"FAIL: Source certificate bundle {host_bundle} not found")
            sys.exit(1)
        build_cmd = [
            sys.executable,
            "scripts/build_trust_anchors.py",
            "--input", host_bundle,
            "--out-bin", ANCHOR_FILE,
            "--out-pem", os.path.join(CERT_DIR, "ca-certificates.crt"),
            "--out-sha256", os.path.join(REPO_ROOT, "assets", "ssl", "ca-certificates.sha256")
        ]
        res = subprocess.run(build_cmd)
        if res.returncode != 0:
            print("FAIL: build_trust_anchors.py failed")
            sys.exit(1)
    else:
        print(f"    -> Using existing trust bundle: {ANCHOR_FILE} ({os.path.getsize(ANCHOR_FILE)} bytes)")

    print("=== [2/5] Compiling library objects with -Os -fstack-usage ===")
    incs = [
        '-I' + os.path.abspath('src/include'),
        '-I' + os.path.abspath('src/lib/tls'),
        '-I' + os.path.abspath('src/lib/tls/bearssl/inc'),
        '-I' + os.path.abspath('src/lib/tls/bearssl/src')
    ]
    srcs = [os.path.abspath(s) for s in CLIENT_SRCS]
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
        print("FAIL: Compilation of library objects failed")
        sys.exit(1)

    print("=== [3/5] Creating static library libfortresstls_anchor.a ===")
    obj_files = [f for f in os.listdir(OBJ_DIR) if f.endswith(".o")]
    if os.path.exists(LIB_OUT):
        os.unlink(LIB_OUT)
    ar_cmd = ["ar", "rcs", LIB_OUT] + obj_files
    res = subprocess.run(ar_cmd, cwd=OBJ_DIR)
    if res.returncode != 0:
        print("FAIL: ar rcs failed")
        sys.exit(1)

    print("=== [4/5] Building TLS anchor host test binary with ASan/UBSan ===")
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
        "-Isrc/lib/tls",
        "-Isrc/lib/tls/bearssl/inc",
        "-Isrc/lib/tls/bearssl/src",
        "-Itests",
        "tests/tls_anchor_host.c",
        LIB_OUT,
        "-o", BIN_OUT
    ]
    res = subprocess.run(test_cmd)
    if res.returncode != 0:
        print("FAIL: Compilation of tests/tls_anchor_host.c failed")
        sys.exit(1)

    print("=== [5/5] Running TLS anchor test suite ===")
    res = subprocess.run([BIN_OUT, ANCHOR_FILE])
    if res.returncode != 0:
        print("FAIL: tls_anchor_host tests failed")
        sys.exit(1)

    print("=== Auditing Stack Usage (-fstack-usage) for anchor parser ===")
    anchor_su = os.path.join(OBJ_DIR, "anchor_parser.su")
    if not os.path.exists(anchor_su):
        print(f"FAIL: Stack usage file {anchor_su} not generated")
        sys.exit(1)

    max_frame = 0
    max_func = ""
    with open(anchor_su, "r") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3:
                loc, size, qual = parts[0], int(parts[1]), parts[2]
                if size > max_frame:
                    max_frame = size
                    max_func = f"{loc} ({size} B)"
                if size > 1024:
                    print(f"FAIL: Stack limit exceeded in {loc}: {size} > 1024 bytes")
                    sys.exit(1)

    print(f"    -> Maximum measured stack frame in anchor_parser: {max_func}")
    print("    -> Stack frame invariant validated: all anchor parser frames <= 1024 B (Gate G1).")
    print("=== Phase TLS-2 Verification PASSED ===")

if __name__ == "__main__":
    main()
