#!/usr/bin/env python3
"""Stage 2 actual-code host tests:
- Name-based download/cache (pkg update, pkg get rot13)
- Ordinary root installation backend (sudo pkg install rot13)
- Retained explicit-directory pkg fetch and pkg install rot13 /staging
- Re-verifying cached artifacts in privileged installation path
- Refusing base-command collisions, foreign files, and cross-package conflicts
- No-op for identical installed build/hash
- Rejection of different version (upgrade required)
- Invalid signature rejection
"""
import datetime
import hashlib
from pathlib import Path
import subprocess
import tempfile
import uuid
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build as app
import pkg_repository as repo

ROOT = Path(__file__).resolve().parents[1]
NOW = 1793188800


def main():
    print("Running Stage 2 host test suite...")
    # 1. Build rot13 v1 ELF and FPK
    subprocess.run(["sh", "examples/pkg/rot13/build.sh"], cwd=ROOT, check=True)
    spec_v1 = ROOT / "examples/pkg/rot13/v1.json"
    spec_v2 = ROOT / "examples/pkg/rot13/v2.json"
    fpk_v1 = app.build_spec(spec_v1)
    fpk_v2 = app.build_spec(spec_v2)

    # 2. Generate TEST Ed25519 keys
    keys = [Ed25519PrivateKey.generate() for _ in range(2)]
    repo_id = bytes(range(16))
    platform_bytes = bytes.fromhex("42" * 32)

    wire_cat, _ = repo.catalog(repo_id, 1, 1, NOW - 100, NOW + 10000, [fpk_v1], keys)

    policy_spec = {
        'repository': str(uuid.UUID(bytes=repo_id)),
        'root_version': 1,
        'platform': platform_bytes.hex(),
        'base_version': 1,
        'keys': [repo.public(k).hex() for k in keys]
    }

    trust_text = 'version=1\n' + ''.join(f'{k}={policy_spec[k]}\n' for k in ('repository', 'root_version', 'platform', 'base_version'))
    trust_text += ''.join('public_key=' + k + '\n' for k in policy_spec['keys'])

    with tempfile.TemporaryDirectory(prefix="pkg-stage2-host-") as temp:
        work = Path(temp)
        trust_file = work / "trust.conf"
        trust_file.write_text(trust_text)

        cat_file = work / "catalog.v1"
        cat_file.write_bytes(wire_cat)

        fpk_file = work / "rot13.fpk"
        fpk_file.write_bytes(fpk_v1)

        # 3. Compile tests/pkg_stage2_host.c with ASan/UBSan
        units = [
            "tests/pkg_stage2_host.c",
            *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "user/pkg").glob("*.c"))],
            "src/lib/crypto/hash.c",
            "src/lib/crypto/curve.c",
            "user/tools/common.c",
            "user/tools/digest.c",
            "user/tools/gzip.c"
        ]

        flags = [
            "gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-no-pie", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-DTOOL_HOST_TEST", "-DFORTRESS_PKG_FETCH=1", "-DFORTRESS_PKG_TRUST=1",
            "-I.", "-Isrc/include", "-Isrc/fs", "-Isrc/kernel", "-Iuser/tools",
            "-Isrc/lib/crypto/upstream/src"
        ]

        # Compile monocypher objects
        mono_objs = []
        for i, src in enumerate(("src/lib/crypto/upstream/src/monocypher.c",
                                "src/lib/crypto/upstream/src/optional/monocypher-ed25519.c")):
            obj = str(work / f"mono-{i}.o")
            subprocess.run(flags + ["-include", "src/lib/crypto/namespace.h", "-c", src, "-o", obj],
                           cwd=ROOT, check=True)
            mono_objs.append(obj)

        bin_path = str(work / "stage2_host")
        subprocess.run(flags + units + mono_objs + ["-o", bin_path], cwd=ROOT, check=True)

        # 4. Run test
        try:
            proc = subprocess.run([bin_path, str(trust_file), str(cat_file), str(fpk_file)],
                                  cwd=ROOT, capture_output=True, text=True, check=True)
            print(proc.stdout)
        except subprocess.CalledProcessError as e:
            print("STDOUT:\n", e.stdout)
            print("STDERR:\n", e.stderr)
            raise

    print("Stage 2 host tests complete: 100% PASS.")


if __name__ == "__main__":
    main()
