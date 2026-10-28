"""Repository publisher interoperability with the actual C catalog verifier.

Temporary TEST signing keys only; no boot media, enrollment or disk devices.
"""
import datetime
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, NoEncryption
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from pkg_artifact_vectors import pack, parts
import pkg_repository as repo

ROOT = Path(__file__).resolve().parents[1]


def rejects(call):
    try:
        call()
    except (ValueError, FileExistsError):
        return
    raise AssertionError("unsafe input accepted")


def main():
    out = ROOT / ".codex-remote-attachments" / ("pkg-repository-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
    out.mkdir(parents=True)
    receipt = {"runtime": "INCOMPLETE", "boot": "NOT RUN", "commands": [], "scope": "publisher and actual C verification; TEST keys only"}

    def run(command, cwd=ROOT):
        index = len(receipt["commands"])
        receipt["commands"].append(command)
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=180)
        (out / f"command-{index}.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr[-3000:]

    sources = ["scripts/pkg_repository.py", "scripts/test_pkg_repository.py", "tests/pkg_catalog_host.c",
               "user/pkg/catalog.c", "user/pkg/format.c", "user/pkg/manifest.c", "user/pkg/verify.c",
               "user/tools/digest.c", "user/tools/gzip.c", "src/lib/crypto/hash.c", "src/lib/crypto/curve.c",
               "src/lib/crypto/upstream/src/monocypher.c", "src/lib/crypto/upstream/src/optional/monocypher-ed25519.c"]
    before = {p: repo.sha((ROOT / p).read_bytes()).hex() for p in sources}
    try:
        with tempfile.TemporaryDirectory(prefix="fortress-repository-test-") as temporary:
            work = Path(temporary)
            publication = work / "public"
            keys = []
            for i in range(2):
                path = work / f"TEST-key-{i}"
                key = Ed25519PrivateKey.generate()
                path.write_bytes(key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption()))
                path.chmod(0o600)
                keys.append(path)
            signers = repo.signing_keys(keys, publication)
            artifact = pack(*parts())
            now, identity = 1793188800, bytes(range(16))
            wire, objects = repo.catalog(identity, 1, 5, now - 1, now + 3600, [artifact], signers)
            repo.publish(publication, wire, objects)
            assert (publication / "catalog.v1").read_bytes() == wire
            for digest, data in objects.items():
                assert (publication / "objects/sha256" / (digest + ".fpk")).read_bytes() == data
            repo.publish(publication, wire, objects)  # identical publication is idempotent
            older, _ = repo.catalog(identity, 1, 4, now - 1, now + 3600, [artifact], signers)
            fork, _ = repo.catalog(identity, 1, 5, now - 2, now + 3600, [artifact], signers)
            rejects(lambda: repo.publish(publication, older, objects))
            rejects(lambda: repo.publish(publication, fork, objects))
            rejects(lambda: repo.catalog(identity, 1, 6, now, now + 3600, [artifact, artifact], signers))
            rejects(lambda: repo.signing_keys(keys + keys[:1], publication))
            keys[0].chmod(0o644)
            rejects(lambda: repo.signing_keys(keys, publication))
            keys[0].chmod(0o600)
            newer, _ = repo.catalog(identity, 1, 6, now - 1, now + 3600, [artifact], signers)
            # A failure before rename retains the old public catalog. A retry
            # may finish the same completely written temporary catalog.
            original_replace = repo.os.replace
            def interrupted(*unused):
                raise OSError("TEST interruption before catalog activation")
            repo.os.replace = interrupted
            try:
                try:
                    repo.publish(publication, newer, objects)
                except OSError:
                    pass
                else:
                    raise AssertionError("interruption did not fire")
            finally:
                repo.os.replace = original_replace
            assert (publication / "catalog.v1").read_bytes() == wire
            repo.publish(publication, newer, objects)
            assert (publication / "catalog.v1").read_bytes() == newer

            fixture = work / "build/pkg-format"
            fixture.mkdir(parents=True)
            (fixture / "fixture.fpk").write_bytes(artifact)
            def policy(floor=0, digest=bytes(32)):
                return struct.pack("<16sQQQ32sII256s", identity, 1, floor, now, digest,
                                   2, 2, b"".join(repo.public(k) for k in signers).ljust(256, b"\0"))
            bad = wire[:-1] + bytes([wire[-1] ^ 1])
            vectors = [(wire, policy(), True), (newer, policy(5, repo.sha(wire)), True),
                       (wire, policy(6, repo.sha(newer)), False),
                       (fork, policy(5, repo.sha(wire)), False), (bad, policy(), False)]
            (fixture / "catalog-vectors.bin").write_bytes(b"".join(
                struct.pack("<BBI", int(ok), int(ok), len(data)) + rules + data for data, rules, ok in vectors))
            flags = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-ffunction-sections",
                     "-fdata-sections", "-I.", "-Isrc/include", "-Isrc/fs", "-Isrc/kernel",
                     "-Iuser/tools", "-Isrc/lib/crypto/upstream/src"]
            objects_native = []
            for i, source in enumerate(sources[-2:]):
                obj = out / f"crypto-{i}.o"
                run(flags + ["-include", "src/lib/crypto/namespace.h", "-c", source, "-o", str(obj)])
                objects_native.append(str(obj))
            probe = out / "catalog-host"
            run(flags + ["-no-pie", "-Wl,--gc-sections", *sources[2:-2], *objects_native, "-o", str(probe)])
            run([str(probe)], cwd=work)
            receipt["catalog_sha256"] = repo.sha(newer).hex()
            receipt["vectors"] = len(vectors)
        receipt["runtime"] = "PASS"
    except BaseException as error:
        receipt["runtime"] = "FAIL"
        receipt["error"] = repr(error)
        raise
    finally:
        receipt["sources"] = before
        receipt["preserved"] = before == {p: repo.sha((ROOT / p).read_bytes()).hex() for p in sources}
        if not receipt["preserved"]:
            receipt["runtime"] = "FAIL"
        (out / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(out, receipt["runtime"], flush=True)
    assert receipt["runtime"] == "PASS"


if __name__ == "__main__":
    main()
