"""Build a deterministic, UNSIGNED PKG-1 desktop candidate.

This is artifact production, never enrollment, catalog signing or authorization.
Platform identity/base range/build sequence must come from release policy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tomllib
from pkg_build import encode, regular, verify

ROOT = Path(__file__).resolve().parents[1]


# PKG-1 installs exactly one executable per package, as /usr/bin/<command>.
# The desktop's apps are therefore separate packages; the compositor runs
# /usr/bin/<app> when installed, else the image's /bin/<app>.
APPS = {
    "desktop": ("fortress-desktop", "build/compositor.elf",
                "FortressOS desktop and built-in settings apps; app API v1"),
    "notepad": ("fortress-notepad", "build/notepad.elf",
                "FortressOS desktop text editor; app API v1"),
    "files": ("fortress-files", "build/files.elf",
              "FortressOS desktop file manager; app API v1"),
}


def build_app(app, elf, platform, sequence, base_min, base_max, version):
    if app == "desktop":
        return build(elf, platform, sequence, base_min, base_max, version)
    name, _, description = APPS[app]
    members = [
        (f"bin/{app}", 1, 0o755, regular(elf, 4 * 1024 * 1024)),
        ("doc/DejaVu-LICENSE.txt", 2, 0o644, regular(ROOT / "user/compositor/assets/DejaVu-LICENSE.txt", 16*1024*1024)),
    ]
    artifact = encode(name, app, version, platform, sequence, base_min, base_max, members, description)
    verify(artifact)
    return artifact


def build(elf, platform, sequence, base_min, base_max, version):
    members = [
        ("bin/desktop", 1, 0o755, regular(elf, 4 * 1024 * 1024)),
        ("doc/DejaVu-LICENSE.txt", 2, 0o644, regular(ROOT / "user/compositor/assets/DejaVu-LICENSE.txt", 16*1024*1024)),
        ("doc/desktop.conf.example", 2, 0o644, regular(ROOT / "assets/desktop.conf", 16*1024*1024)),
        ("doc/desktop.md", 2, 0o644, regular(ROOT / "docs/subsystems/desktop.md", 16*1024*1024)),
    ]
    artifact = encode("fortress-desktop", "desktop", version, platform, sequence, base_min, base_max,
                      members, "FortressOS desktop and built-in settings apps; app API v1")
    verify(artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", choices=sorted(APPS), default="desktop")
    parser.add_argument("--elf", type=Path, help="default: the app's build/*.elf")
    parser.add_argument("--platform-id", required=True, help="release-policy platform SHA-256, 64 hex characters")
    parser.add_argument("--build", type=int, required=True)
    parser.add_argument("--base-min", type=int, required=True)
    parser.add_argument("--base-max", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    version = tomllib.loads((ROOT / "user/compositor/Cargo.toml").read_text())["package"]["version"]
    package, default_elf, _ = APPS[args.app]
    elf = args.elf or ROOT / default_elf
    artifact = build_app(args.app, elf, bytes.fromhex(args.platform_id), args.build, args.base_min,
                         args.base_max, version)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(artifact)
    checksum = hashlib.sha256(artifact).hexdigest()
    args.out.with_suffix(args.out.suffix + ".sha256").write_text(f"{checksum}  {args.out.name}\n")
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps({
        "package": package, "command": args.app, "version": version,
        "app_api": 1, "build": args.build, "platform_id": args.platform_id,
        "base_min": args.base_min, "base_max": args.base_max, "sha256": checksum,
        "bytes": len(artifact), "authorization": "UNSIGNED; requires approved catalog and enrolled root",
    }, indent=2) + "\n")
    print(f"UNSIGNED {args.app} {version} candidate: {args.out} ({len(artifact)} bytes)")


if __name__ == "__main__":
    main()
