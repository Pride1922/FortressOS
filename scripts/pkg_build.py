"""Build/inspect deterministic unsigned FPK v1 apps on a Linux host (GCC required)."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDENTITY = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def span(text):
    data = text.encode("ascii")
    return struct.pack("<H", len(data)) + data


def regular(path, limit):
    # Reject links in every component, including JSON-relative parent directories.
    path = Path(os.path.abspath(path))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError("linked input refused")
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                info.st_mode & 0o6000 or not 0 < info.st_size <= limit):
            raise ValueError("expected bounded nonempty regular file without links/set-ID")
        data = stream.read(limit + 1)
        if len(data) != info.st_size:
            raise ValueError("input changed size")
        return data


def verify(data, sanitize=False):
    """Use the shipped C manifest, gzip, hash and ELF validators, not a policy clone."""
    with tempfile.TemporaryDirectory(prefix="fortress-fpk-verify-") as temporary:
        work = Path(temporary)
        runner = work / "verify"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if sanitize else []
        subprocess.run(["gcc", *flags, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-I.", "-Isrc/include", "-Isrc/fs", "-Isrc/kernel", "-Iuser/tools",
                        "scripts/pkg_verify_host.c", "user/pkg/format.c", "user/pkg/manifest.c",
                        "user/pkg/verify.c", "user/tools/digest.c", "user/tools/gzip.c",
                        "-o", str(runner)], cwd=ROOT, check=True)
        artifact = work / "candidate.fpk"
        artifact.write_bytes(data)
        subprocess.run([str(runner), str(artifact), hashlib.sha256(data).hexdigest()], check=True)


def encode(name, command, version, platform, sequence, base_min, base_max, members, description=""):
    for identity in (name, command):
        if not isinstance(identity, str) or len(identity) > 32 or not IDENTITY.fullmatch(identity):
            raise ValueError("invalid package/command identity")
    if len(platform) != 32 or not any(platform):
        raise ValueError("platform identity must be nonzero and 32 bytes")
    if (any(type(v) is not int for v in (sequence, base_min, base_max)) or
            not 0 < sequence < 2**64 or not 0 <= base_min <= base_max < 2**64):
        raise ValueError("invalid build/base range")
    for text, minimum, maximum in ((version, 1, 32), (description, 0, 256)):
        if not isinstance(text, str) or not minimum <= len(text) <= maximum or any(not 32 <= ord(c) <= 126 for c in text):
            raise ValueError("invalid version/description")
    if not 1 <= len(members) <= 16:
        raise ValueError("one executable and at most fifteen doc/data files required")
    members = sorted(members)
    if 1024 + sum(512 + ((len(data)+511)//512)*512 for _, _, _, data in members) > 16*1024*1024:
        raise ValueError("expanded archive ceiling exceeded")
    paths = []
    for path, role, mode, data in members:
        if (not isinstance(path, str) or not 1 <= len(path) <= 100 or
                any(not 33 <= ord(c) <= 126 or c in "\\:" for c in path) or
                any(not part or len(part) > 63 or part in (".", "..") for part in path.split("/")) or
                path in paths or any(path.startswith(prior + "/") for prior in paths)):
            raise ValueError("invalid/duplicate/conflicting payload path")
        if not data or (role == 1 and (path != "bin/" + command or mode != 0o755 or len(data) > 4*1024*1024)) or (role == 2 and (not path.startswith("doc/") or mode != 0o644)) or role not in (1, 2):
            raise ValueError("unsupported payload role/mode/size")
        paths.append(path)
    if sum(role == 1 for _, role, _, _ in members) != 1:
        raise ValueError("exactly one executable required")
    manifest = struct.pack("<4sHHQQQ32s", b"FMN1", 1, len(members), sequence, base_min, base_max, platform)
    manifest += span(name) + span(command) + span(version)
    manifest += span(description)
    archive = bytearray()
    for name, role, mode, data in members:
        manifest += struct.pack("<BBHQ32s", role, 0, mode, len(data), hashlib.sha256(data).digest()) + span(name)
        info = tarfile.TarInfo(name)
        info.size, info.mode, info.uid, info.gid, info.mtime = len(data), mode, 0, 0, 0
        info.uname = info.gname = ""
        archive += info.tobuf(format=tarfile.USTAR_FORMAT) + data
        archive += b"\0" * (-len(data) % 512)
    archive += b"\0" * 1024  # Exactly two terminators; no tarfile record padding.
    payload = gzip.compress(bytes(archive), mtime=0)
    if len(manifest) > 16384 or len(archive) > 16 * 1024 * 1024 or len(payload) > 8 * 1024 * 1024:
        raise ValueError("PKG-1 size ceiling exceeded")
    header = struct.pack("<8sIIIIIIQQ32s32s16s", b"FORTFPK\0", 1, 128, 0, len(manifest), 0, 0,
                         len(payload), len(archive), hashlib.sha256(manifest).digest(),
                         hashlib.sha256(payload).digest(), bytes(16))
    return header + manifest + payload


def build_spec(path):
    path = Path(path).absolute()
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result
    spec = json.loads(regular(path, 65536), object_pairs_hook=unique)
    required = {"name", "command", "version", "build", "platform_id", "base_min", "base_max", "executable"}
    if not isinstance(spec, dict) or not required <= spec.keys() or spec.keys() - required - {"files", "description"}:
        raise ValueError("missing/unknown app fields")
    files = spec.get("files", [])
    if not isinstance(files, list) or len(files) > 15:
        raise ValueError("files must be a list of at most fifteen entries")
    members = [("bin/" + spec["command"], 1, 0o755, regular(path.parent / spec["executable"], 4*1024*1024))]
    for item in files:
        if not isinstance(item, dict) or set(item) != {"source", "path"}:
            raise ValueError("file entry requires source and path")
        members.append((item["path"], 2, 0o644, regular(path.parent / item["source"], 16*1024*1024)))
    return encode(spec["name"], spec["command"], spec["version"], bytes.fromhex(spec["platform_id"]),
                  spec["build"], spec["base_min"], spec["base_max"], members, spec.get("description", ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    build = sub.add_parser("build")
    build.add_argument("spec", type=Path)
    build.add_argument("--out", type=Path, required=True)
    inspect = sub.add_parser("inspect")
    inspect.add_argument("artifact", type=Path)
    args = parser.parse_args()
    data = build_spec(args.spec) if args.action == "build" else regular(args.artifact, 128+16384+8*1024*1024)
    verify(data)
    if args.action == "build":
        args.out.parent.mkdir(parents=True, exist_ok=True)
        # Refuse replacement, including symlinks; releases are explicit artifacts.
        with args.out.open("xb") as stream:
            stream.write(data)
    print(json.dumps({"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                      "authorization": "unsigned artifact; signed catalog required"}))


if __name__ == "__main__":
    main()
