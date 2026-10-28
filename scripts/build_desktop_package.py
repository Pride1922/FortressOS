"""Build a deterministic, UNSIGNED PKG-1 desktop candidate.

This is artifact production, never enrollment, catalog signing or authorization.
Platform identity/base range/build sequence must come from release policy.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def span(text):
    data = text.encode("ascii")
    return struct.pack("<H", len(data)) + data


def build(elf, platform, sequence, base_min, base_max, version):
    if len(platform) != 32 or not any(platform):
        raise ValueError("platform identity must be a nonzero 32-byte value")
    if not (0 < sequence < 2**64 and 0 <= base_min <= base_max < 2**64):
        raise ValueError("invalid build sequence/base range")
    if not version or len(version) > 32 or any(ord(c) < 32 or ord(c) > 126 for c in version):
        raise ValueError("invalid version")
    executable = Path(elf).read_bytes()
    if not 0 < len(executable) <= 4 * 1024 * 1024 or executable[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a <=4 MiB freestanding x86_64 ELF")
    members = sorted([
        ("bin/desktop", 1, 0o755, executable),
        ("doc/DejaVu-LICENSE.txt", 2, 0o644, (ROOT / "user/compositor/assets/DejaVu-LICENSE.txt").read_bytes()),
        ("doc/desktop.conf.example", 2, 0o644, (ROOT / "assets/desktop.conf").read_bytes()),
        ("doc/desktop.md", 2, 0o644, (ROOT / "docs/subsystems/desktop.md").read_bytes()),
    ])
    manifest = struct.pack("<4sHHQQQ32s", b"FMN1", 1, len(members), sequence, base_min, base_max, platform)
    manifest += span("fortress-desktop") + span("desktop") + span(version)
    manifest += span("FortressOS desktop and built-in settings apps; app API v1")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=ROOT / "build/compositor.elf")
    parser.add_argument("--platform-id", required=True, help="release-policy platform SHA-256, 64 hex characters")
    parser.add_argument("--build", type=int, required=True)
    parser.add_argument("--base-min", type=int, required=True)
    parser.add_argument("--base-max", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    version = tomllib.loads((ROOT / "user/compositor/Cargo.toml").read_text())["package"]["version"]
    artifact = build(args.elf, bytes.fromhex(args.platform_id), args.build, args.base_min, args.base_max, version)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(artifact)
    checksum = hashlib.sha256(artifact).hexdigest()
    args.out.with_suffix(args.out.suffix + ".sha256").write_text(f"{checksum}  {args.out.name}\n")
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps({
        "package": "fortress-desktop", "command": "desktop", "version": version,
        "app_api": 1, "build": args.build, "platform_id": args.platform_id,
        "base_min": args.base_min, "base_max": args.base_max, "sha256": checksum,
        "bytes": len(artifact), "authorization": "UNSIGNED; requires approved catalog and enrolled root",
    }, indent=2) + "\n")
    print(f"UNSIGNED desktop {version} candidate: {args.out} ({len(artifact)} bytes)")


if __name__ == "__main__":
    main()
