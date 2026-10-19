#!/usr/bin/env python3
"""
scripts/build_trust_anchors.py
Compiles Mozilla / system PEM trust anchor bundle into:
  1. Binary anchors.v1 table adhering to docs/plans/TLS_PLAN.md §4.2.
  2. Curated PEM certificate bundle for initramfs / distribution.
  3. SHA-256 integrity digest in assets/ssl/ca-certificates.sha256.
  4. Optional bootstrap C header (src/lib/tls/trust_anchors.h).
"""

import argparse
import hashlib
import os
import struct
import sys
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec

TLS_ANCHOR_MAGIC = b"FOSANC01"
TLS_ANCHOR_VERSION = 1
MAX_ANCHORS = 256
MAX_FILE_SIZE = 262144
MAX_DN_LEN = 1024

BR_KEYTYPE_RSA = 1
BR_KEYTYPE_EC = 2

BR_EC_secp256r1 = 23
BR_EC_secp384r1 = 24
BR_EC_curve25519 = 29

BR_X509_TA_CA = 1

def parse_args():
    parser = argparse.ArgumentParser(description="Compile TLS Trust Anchors for FortressOS")
    parser.add_argument("--input", "-i", default="/etc/ssl/certs/ca-certificates.crt",
                        help="Input PEM CA bundle path (default: /etc/ssl/certs/ca-certificates.crt)")
    parser.add_argument("--out-bin", "-b", required=True,
                        help="Output binary anchors.v1 path")
    parser.add_argument("--out-pem", "-p", default=None,
                        help="Output curated PEM bundle path")
    parser.add_argument("--out-hash", "--out-sha256", dest="out_hash", default=None,
                        help="Output SHA-256 digest path")
    parser.add_argument("--out-header", default=None,
                        help="Output C bootstrap header path")
    return parser.parse_args()

def main():
    args = parse_args()

    if not os.path.exists(args.input):
        print(f"Error: input file '{args.input}' not found", file=sys.stderr)
        sys.exit(1)

    with open(args.input, "rb") as f:
        pem_data = f.read()

    input_sha256 = hashlib.sha256(pem_data).hexdigest()
    print(f"Input PEM bundle size: {len(pem_data)} bytes, SHA-256: {input_sha256}")

    certs = x509.load_pem_x509_certificates(pem_data)
    print(f"Parsed {len(certs)} X.509 certificates from input bundle.")

    valid_anchors = []
    included_certs = []

    for cert in certs:
        # Check basic constraints: must be a CA
        try:
            bc = cert.extensions.get_extension_for_oid(x509.oid.ExtensionOID.BASIC_CONSTRAINTS).value
            if not bc.ca:
                continue
        except x509.ExtensionNotFound:
            pass  # Root anchors might not strictly include basic constraints in legacy certs, but usually do

        dn = cert.subject.public_bytes(serialization.Encoding.DER)
        if len(dn) == 0 or len(dn) > MAX_DN_LEN:
            continue

        pk = cert.public_key()
        if isinstance(pk, rsa.RSAPublicKey):
            n = pk.public_numbers().n
            e = pk.public_numbers().e
            n_bytes = n.to_bytes((n.bit_length() + 7) // 8, 'big')
            e_bytes = e.to_bytes((e.bit_length() + 7) // 8, 'big')

            # Validate RSA modulus: 2048 to 4096 bits
            if not (2048 <= pk.key_size <= 4096):
                continue
            if len(n_bytes) < 256 or len(n_bytes) > 512:
                continue

            # Validate exponent: odd, >= 3, <= 4 bytes
            if e < 3 or (e % 2) == 0 or len(e_bytes) > 4:
                continue

            valid_anchors.append({
                'type': BR_KEYTYPE_RSA,
                'dn': dn,
                'flags': BR_X509_TA_CA,
                'n': n_bytes,
                'e': e_bytes,
                'name': cert.subject.rfc4514_string()
            })
            included_certs.append(cert)

        elif isinstance(pk, ec.EllipticCurvePublicKey):
            if pk.curve.name == 'secp256r1':
                cid = BR_EC_secp256r1
            elif pk.curve.name == 'secp384r1':
                cid = BR_EC_secp384r1
            else:
                # Unsupported curve (e.g. secp521r1 or sect)
                continue

            q_bytes = pk.public_bytes(
                serialization.Encoding.X962,
                serialization.PublicFormat.UncompressedPoint
            )

            valid_anchors.append({
                'type': BR_KEYTYPE_EC,
                'dn': dn,
                'flags': BR_X509_TA_CA,
                'curve': cid,
                'q': q_bytes,
                'name': cert.subject.rfc4514_string()
            })
            included_certs.append(cert)

    print(f"Selected {len(valid_anchors)} compliant root trust anchors.")
    if len(valid_anchors) > MAX_ANCHORS:
        print(f"Error: anchor count {len(valid_anchors)} exceeds maximum {MAX_ANCHORS}", file=sys.stderr)
        sys.exit(1)

    if len(valid_anchors) == 0:
        print("Error: zero compliant trust anchors found", file=sys.stderr)
        sys.exit(1)

    # Build binary table
    count = len(valid_anchors)
    table_offset = 16
    payload_start = table_offset + count * 32

    descriptors = []
    payload = bytearray()

    for anc in valid_anchors:
        dn_bytes = anc['dn']
        dn_offset = payload_start + len(payload)
        payload.extend(dn_bytes)
        dn_len = len(dn_bytes)

        flags = anc['flags']
        key_type = anc['type']

        if key_type == BR_KEYTYPE_RSA:
            n_bytes = anc['n']
            e_bytes = anc['e']

            n_offset = payload_start + len(payload)
            payload.extend(n_bytes)
            n_len = len(n_bytes)

            e_offset = payload_start + len(payload)
            payload.extend(e_bytes)
            e_len = len(e_bytes)

            # Descriptor format: dn_offset(4), dn_len(4), flags(4), key_type(4),
            #                    n_offset(4), n_len(4), e_offset(4), e_len(4)
            desc = struct.pack(
                "<IIIIIIII",
                dn_offset, dn_len, flags, key_type,
                n_offset, n_len, e_offset, e_len
            )
            descriptors.append(desc)
        else:
            q_bytes = anc['q']
            curve_id = anc['curve']

            q_offset = payload_start + len(payload)
            payload.extend(q_bytes)
            q_len = len(q_bytes)

            # Descriptor format: dn_offset(4), dn_len(4), flags(4), key_type(4),
            #                    curve_id(4), q_offset(4), q_len(4), pad(4)
            desc = struct.pack(
                "<IIIIIIII",
                dn_offset, dn_len, flags, key_type,
                curve_id, q_offset, q_len, 0
            )
            descriptors.append(desc)

    header = struct.pack("<8sII", TLS_ANCHOR_MAGIC, TLS_ANCHOR_VERSION, count)
    bin_content = header + b"".join(descriptors) + bytes(payload)

    if len(bin_content) > MAX_FILE_SIZE:
        print(f"Error: binary file size {len(bin_content)} exceeds maximum {MAX_FILE_SIZE}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.out_bin)), exist_ok=True)
    with open(args.out_bin, "wb") as f:
        f.write(bin_content)
    print(f"Wrote binary trust anchors: {args.out_bin} ({len(bin_content)} bytes)")

    if args.out_pem:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_pem)), exist_ok=True)
        with open(args.out_pem, "wb") as f:
            for c in included_certs:
                f.write(c.public_bytes(serialization.Encoding.PEM))
        print(f"Wrote curated PEM bundle: {args.out_pem}")

    if args.out_hash:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_hash)), exist_ok=True)
        with open(args.out_hash, "w") as f:
            f.write(f"{input_sha256}  {os.path.basename(args.input)}\n")
        print(f"Wrote SHA-256 digest: {args.out_hash}")

    if args.out_header:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_header)), exist_ok=True)
        with open(args.out_header, "w") as f:
            f.write("/* Auto-generated bootstrap trust anchors for FortressOS */\n")
            f.write("#ifndef FORTRESS_TLS_TRUST_ANCHORS_H\n#define FORTRESS_TLS_TRUST_ANCHORS_H\n\n")
            f.write('#include "tls.h"\n\n')
            f.write(f"#define BOOTSTRAP_TRUST_ANCHOR_COUNT {count}\n\n")
            f.write(f"static const uint8_t s_bootstrap_anchors_v1[{len(bin_content)}] = {{\n")
            for idx, byte in enumerate(bin_content):
                if idx % 16 == 0:
                    f.write("    ")
                f.write(f"0x{byte:02x}, ")
                if (idx + 1) % 16 == 0:
                    f.write("\n")
            if len(bin_content) % 16 != 0:
                f.write("\n")
            f.write("};\n\n")
            f.write("#endif /* FORTRESS_TLS_TRUST_ANCHORS_H */\n")
        print(f"Wrote bootstrap header: {args.out_header}")

    print("Trust anchor compilation complete.")

if __name__ == "__main__":
    main()
