"""Stage 1 host-only tests. Fixed public TEST seed in memory; no secrets provisioned."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build as app
import pkg_repository as repo


def rejects(call):
    try:
        call()
    except (ValueError, OSError, subprocess.CalledProcessError):
        return
    raise AssertionError("invalid input accepted")


def main():
    subprocess.run(["sh", "examples/pkg/rot13/build.sh"], cwd=app.ROOT, check=True)
    # Exercise actual tool logic on a host adapter, not Linux syscalls in the ELF.
    with tempfile.TemporaryDirectory(prefix="rot13-host-") as temp:
        adapter = Path(temp) / "adapter.c"
        adapter.write_text("""
#include <stdio.h>
#include <string.h>
#include "common.h"
long tool_syscall(long n, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; if(n!=SYS_READ) return -1;
    size_t got=fread((void *)b,1,c,stdin); return ferror(stdin)?-1:(long)got;
}
bool tool_equal(const char *a,const char *b) { return !strcmp(a,b); }
int tool_write(const char *t,const void *p,size_t n) { (void)t; return fwrite(p,1,n,stdout)!=n; }
int tool_error(const char *t,const char *m,const char *p) { (void)t;(void)m;(void)p;return 1; }
int tool_sys_error(const char *t,const char *m,const char *p,long e) { (void)e;return tool_error(t,m,p); }
int rot13_main(int,char **);
int main(int argc,char **argv) { return rot13_main(argc,argv); }
""")
        for release in (1, 2):
            runner = Path(temp) / "rot13"
            subprocess.run(["gcc", "-fsanitize=address,undefined", "-no-pie", "-Wall", "-Wextra", "-Werror",
                            "-Isrc/include", "-Isrc/fs", "-Iuser/tools", f"-DRELEASE={release}",
                            str(adapter), "examples/pkg/rot13/rot13.c", "-o", str(runner)], cwd=app.ROOT, check=True)
            raw = bytes(range(256))*40
            expected = bytes(c+13 if 65<=c<=77 or 97<=c<=109 else c-13 if 78<=c<=90 or 110<=c<=122 else c for c in raw)
            result = subprocess.run([str(runner)], input=raw, capture_output=True, check=True)
            assert result.stdout == expected
            assert subprocess.check_output([str(runner), "--version"]) == ("rot13 1.0\n" if release == 1 else "rot13 2.0\n").encode()
            help = subprocess.run([str(runner), "--help"], capture_output=True)
            assert help.returncode == (1 if release == 1 else 0)
            if release == 2:
                assert b"Usage: rot13" in help.stdout
    specs = [app.ROOT / f"examples/pkg/rot13/v{i}.json" for i in (1, 2)]
    v1, v2 = [app.build_spec(p) for p in specs]
    assert v1 == app.build_spec(specs[0]) and v1 != v2
    app.verify(v1, sanitize=True)
    app.verify(v2, sanitize=True)
    rejects(lambda: app.verify(v2[:-1] + bytes([v2[-1] ^ 1])))
    with tempfile.TemporaryDirectory(prefix="pkg-stage1-") as temp:
        work = Path(temp)
        elf = work / "app.elf"
        elf.write_bytes((app.ROOT / "build/pkg-rot13/rot13-v1.elf").read_bytes())
        spec = json.loads(specs[0].read_text())
        spec.update(executable="app.elf", files=[])
        config = work / "app.json"
        def candidate(**changes):
            config.write_text(json.dumps(dict(spec, **changes)))
            data = app.build_spec(config)
            app.verify(data)
            return data
        candidate()
        for changes in [dict(name="../bad"), dict(build=0), dict(build=True), dict(platform_id="00"*32),
                        dict(base_max=0), dict(version="\n"), dict(unknown=1),
                        dict(files=[dict(source="app.elf", path="doc/../bad")]),
                        dict(files=[dict(source="app.elf", path="bin/other")]),
                        dict(files=[dict(source="app.elf", path="doc/a")]*2),
                        dict(files=[dict(source="app.elf", path="doc/a"), dict(source="app.elf", path="doc/a/b")])]:
            rejects(lambda changes=changes: candidate(**changes))
        link = work / "link"
        link.symlink_to(elf)
        rejects(lambda: candidate(executable="link"))
        link.unlink()
        import os
        os.link(elf, link)
        rejects(lambda: candidate())
        link.unlink()
        elf.chmod(0o4755)
        rejects(lambda: candidate())
        elf.chmod(0o644)
        fifo = work / "fifo"
        os.mkfifo(fifo)
        rejects(lambda: candidate(executable="fifo"))
        elf.write_bytes(b"not an ELF")
        rejects(lambda: candidate())
        # Retain an unrelated offer; replace rot13 only, using the existing format.
        other = app.encode("another-tool", "rot13", "1.0", bytes([0x42])*32, 1, 1, 1,
                           [("bin/rot13", 1, 0o755, (app.ROOT / "build/pkg-rot13/rot13-v1.elf").read_bytes())])
        keys = [Ed25519PrivateKey.from_private_bytes(bytes(range(32)))]  # PUBLIC TEST vector
        identity, now = bytes(range(16)), 1793350800
        public = work / "public"
        def catalog(revision, artifacts):
            return repo.catalog(identity, 1, revision, now, now+3600, artifacts, keys)
        old, objects = catalog(1, [v1, other])
        repo.publish(public, old, objects)
        artifacts, expected = repo.retained_artifacts(public, [v2], keys, identity, 1)
        assert len(artifacts) == 2 and other in artifacts and v1 not in artifacts
        wire, objects = catalog(2, artifacts)
        with patch.object(repo.os, "replace", side_effect=OSError("cut before activation")):
            rejects(lambda: repo.publish(public, wire, objects, expected))
        assert (public / "catalog.v1").read_bytes() == old
        repo.publish(public, wire, objects, expected)
        assert (public / "catalog.v1").read_bytes() == wire
        rejects(lambda: repo.publish(public, wire, objects, expected))  # stale snapshot
        rejects(lambda: repo.retained_artifacts(public, [v1], keys, identity, 1))
        executable = (app.ROOT / "build/pkg-rot13/rot13-v1.elf").read_bytes()
        substitution = app.encode("rot13", "rot13", "changed", bytes([0x42])*32, 2, 1, 1,
                                  [("bin/rot13", 1, 0o755, executable)])
        rejects(lambda: repo.retained_artifacts(public, [substitution], keys, identity, 1))
        retained_path = public / "objects/sha256" / (repo.sha(other).hex() + ".fpk")
        retained_path.write_bytes(b"corrupt")
        rejects(lambda: repo.retained_artifacts(public, [v2], keys, identity, 1))
        assert (public / "catalog.v1").read_bytes() == wire
        retained_path.write_bytes(other)
        altered = bytearray(v2); altered[-1] ^= 1
        rejects(lambda: repo.retained_artifacts(public, [bytes(altered)], keys, identity, 1))
        bad = bytearray(wire); bad[-1] ^= 1
        (public / "catalog.v1").write_bytes(bad)
        try:
            repo.retained_artifacts(public, [v2], keys, identity, 1)
        except Exception as error:
            from cryptography.exceptions import InvalidSignature
            assert isinstance(error, InvalidSignature)
        else:
            raise AssertionError("invalid signature accepted")
    print("Stage 1 determinism, actual C verification, payload rejection, retention, rollback and publication cuts PASS")


if __name__ == "__main__":
    main()
