"""TEST identities only; never enrolls or writes a filesystem image."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
from build_desktop_package import ROOT, build


def main():
    runner = ROOT / "build/desktop-package-host"
    flags = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I.", "-Isrc/include", "-Isrc/fs", "-Isrc/kernel", "-Iuser/tools"]
    subprocess.run(flags + ["tests/desktop_package_host.c", "user/pkg/format.c", "user/pkg/manifest.c",
                           "user/pkg/verify.c", "user/tools/digest.c", "user/tools/gzip.c",
                           "-o", str(runner)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="fortress-desktop-package-") as temporary:
        path = Path(temporary) / "TEST-desktop.fpk"
        arguments = (ROOT / "build/compositor.elf", bytes([0x42]) * 32, 1, 1, 1, "0.2.0")
        data = build(*arguments)
        assert data == build(*arguments), "nondeterministic package"
        path.write_bytes(data)
        checksum = hashlib.sha256(data).hexdigest()
        subprocess.run([str(runner), str(path), checksum], check=True)
        bad = bytearray(data); bad[-20] ^= 1; path.write_bytes(bad)
        assert subprocess.run([str(runner), str(path), checksum]).returncode != 0
        for platform in [bytes(32), bytes(31)]:
            try:
                build(arguments[0], platform, *arguments[2:])
            except ValueError:
                pass
            else:
                raise AssertionError("invalid platform identity accepted")
    print("Desktop package determinism, format/ELF verification and corruption rejection PASS (TEST identity)")


if __name__ == "__main__":
    main()
