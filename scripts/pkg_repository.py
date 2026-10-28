"""Produce signed PKG-1 catalogs and content-addressed repository objects.

Offline signing tool: never generates keys, enrolls roots or opens devices.
The HTTP server needs only the resulting public directory, never signing keys.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import uuid

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[1]
MIN_TIME, MAX_TIME = 1767225600, 4102444800
MAX_ARTIFACT = 128 + 16384 + 8 * 1024 * 1024
NAME = re.compile(rb"[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def sha(data):
    return hashlib.sha256(data).digest()


def regular(path, limit, private=False):
    """Open without following a final symlink, then check the retained object."""
    path = Path(path)
    if path.is_symlink():
        raise ValueError("symlink input refused")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            raise ValueError("bounded regular input required")
        if private and (info.st_nlink != 1 or info.st_mode & 0o077):
            raise ValueError("private key must have one link and mode 0600 or stricter")
        data = stream.read(limit + 1)
        if len(data) != info.st_size:
            raise ValueError("input changed size while reading")
        return data


def public(key):
    return key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def signing_keys(paths, output):
    result = []
    for path in paths:
        resolved = Path(path).resolve(strict=True)
        if resolved.is_relative_to(ROOT) or resolved.is_relative_to(Path(output).resolve()):
            raise ValueError("signing seeds must remain outside source and publication trees")
        seed = regular(path, 32, private=True)
        if len(seed) != 32:
            raise ValueError("expected an external 32-byte Ed25519 seed")
        key = Ed25519PrivateKey.from_private_bytes(seed)
        if any(public(key) == public(other) for other in result):
            raise ValueError("duplicate signing key")
        result.append(key)
    if not 1 <= len(result) <= 8:
        raise ValueError("one through eight signing keys required")
    return result


def artifact_identity(data):
    """Check envelope/manifest identity; target must still run the full verifier."""
    if not 128 <= len(data) <= MAX_ARTIFACT:
        raise ValueError("artifact length outside PKG-1 bounds")
    fields = struct.unpack_from("<8sIIIIIIQQ32s32s16s", data)
    magic, version, header, flags, mn, r0, r1, compressed, expanded, mh, ph, tail = fields
    if (magic != b"FORTFPK\0" or version != 1 or header != 128 or flags or r0 or r1
            or tail != bytes(16) or not 1 <= mn <= 16384
            or not 1 <= compressed <= 8 * 1024 * 1024
            or not 1 <= expanded <= 16 * 1024 * 1024
            or len(data) != 128 + mn + compressed):
        raise ValueError("invalid PKG-1 envelope")
    manifest = data[128:128 + mn]
    if sha(manifest) != mh or sha(data[128 + mn:]) != ph or len(manifest) < 66:
        raise ValueError("artifact integrity failure")
    magic, version, count, build, base_min, base_max, platform = struct.unpack_from("<4sHHQQQ32s", manifest)
    if (magic != b"FMN1" or version != 1 or not 1 <= count <= 16 or not build
            or base_min > base_max or not any(platform)):
        raise ValueError("invalid manifest header")
    length = struct.unpack_from("<H", manifest, 64)[0]
    name = manifest[66:66 + length]
    if not 1 <= length <= 32 or len(name) != length or not NAME.fullmatch(name):
        raise ValueError("invalid package identity")
    return name, build


def catalog(repository, root_version, revision, issued, expires, artifacts, keys):
    if not any(repository) or len(repository) != 16:
        raise ValueError("nonzero repository UUID required")
    if not 0 < root_version < 2**64 or not 0 < revision < 2**64:
        raise ValueError("nonzero bounded root/catalog versions required")
    if not MIN_TIME <= issued < expires <= MAX_TIME:
        raise ValueError("invalid catalog validity interval")
    if not 1 <= len(artifacts) <= 256 or not 1 <= len(keys) <= 8:
        raise ValueError("catalog entry/signer bounds exceeded")
    entries, objects = {}, {}
    for data in artifacts:
        name, build = artifact_identity(data)
        if name in entries:
            raise ValueError("duplicate package")
        digest = sha(data)
        entries[name] = bytes([len(name)]) + name.ljust(32, b"\0") + struct.pack("<QQ", build, len(data)) + digest
        objects[digest.hex()] = data
    body = struct.pack("<16s16sQQQQHHI", b"FORT-PKG-CAT-v1\0", repository,
                       root_version, revision, issued, expires, len(entries), len(keys), 0)
    body += b"".join(entries[name] for name in sorted(entries))
    signatures = sorted((sha(public(key)), key.sign(body)) for key in keys)
    if len({identity for identity, _ in signatures}) != len(keys):
        raise ValueError("duplicate catalog signer")
    return body + b"".join(identity + signature for identity, signature in signatures), objects


def immutable(path, data):
    """Content-addressed objects cannot be replaced with different bytes."""
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError:
        if regular(path, max(len(data), 1)) != data:
            raise ValueError("existing publication object differs")
        return
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def directory_barrier(path):
    if os.name != "posix":
        raise ValueError("durable repository publication requires a POSIX host")
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def publish(output, wire, objects):
    """Publish objects first; atomic catalog rename is the visibility point."""
    output = Path(output)
    if os.name != "posix":
        raise ValueError("durable repository publication requires a POSIX host")
    output.mkdir(parents=True, exist_ok=True)
    target = output / "objects" / "sha256"
    target.mkdir(parents=True, exist_ok=True)
    if output.is_symlink() or (output / "objects").is_symlink() or target.is_symlink():
        raise ValueError("publication directories must not be symlinks")
    lock = output / ".publish.lock"
    fd = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    temporary = output / ".catalog.v1.new"
    try:
        revision = struct.unpack_from("<Q", wire, 40)[0]
        current = output / "catalog.v1"
        if current.exists() or current.is_symlink():
            old = regular(current, 256 * 1024)
            if len(old) < 72 or old[:16] != b"FORT-PKG-CAT-v1\0" or old[16:40] != wire[16:40]:
                raise ValueError("existing catalog repository/root differs")
            previous = struct.unpack_from("<Q", old, 40)[0]
            if revision < previous or (revision == previous and old != wire):
                raise ValueError("catalog rollback or equal-revision substitution")
        for digest, data in objects.items():
            immutable(target / (digest + ".fpk"), data)
        directory_barrier(target)
        directory_barrier(target.parent)
        directory_barrier(output)
        immutable(temporary, wire)
        os.replace(temporary, current)
        directory_barrier(output)
    finally:
        lock.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True, type=uuid.UUID)
    parser.add_argument("--root-version", required=True, type=int)
    parser.add_argument("--revision", required=True, type=int)
    parser.add_argument("--issued", required=True, type=int, help="UTC Unix seconds")
    parser.add_argument("--expires", required=True, type=int, help="UTC Unix seconds")
    parser.add_argument("--key", action="append", required=True, type=Path)
    parser.add_argument("--artifact", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    keys = signing_keys(args.key, args.output)
    wire, objects = catalog(args.repository.bytes, args.root_version, args.revision,
                            args.issued, args.expires,
                            [regular(path, MAX_ARTIFACT) for path in args.artifact], keys)
    publish(args.output, wire, objects)
    print(json.dumps({"catalog_sha256": sha(wire).hex(), "revision": args.revision,
                      "objects": sorted(objects), "signers": [sha(public(k)).hex() for k in keys]}))


if __name__ == "__main__":
    main()
