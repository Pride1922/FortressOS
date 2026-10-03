#!/usr/bin/env python3
"""Actual fixed-memory codecs and CLI; independent hashlib oracle, mocked I/O."""
import ctypes
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "checksum-host"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O2",
             "-Isrc/include", "-Isrc/fs", "-Iuser/tools"]
    sources = ["user/tools/digest.c", "user/tools/checksum.c", "user/tools/common.c",
               "tests/checksum_host.c"]
    exe = BUILD / "tests"
    subprocess.run(flags + ["-DTOOL_HOST_TEST", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-no-pie"] + sources + ["-o", str(exe)],
                   cwd=ROOT, check=True)
    subprocess.run([exe], check=True)
    lib_path = BUILD / "digest.so"
    subprocess.run(flags + ["-shared", "-fPIC", "user/tools/digest.c", "-o", str(lib_path)],
                   cwd=ROOT, check=True)
    lib = ctypes.CDLL(str(lib_path))

    class Context(ctypes.Structure):
        _fields_ = [("state", ctypes.c_uint32 * 8), ("bytes", ctypes.c_uint64),
                    ("block", ctypes.c_uint8 * 64), ("used", ctypes.c_size_t),
                    ("kind", ctypes.c_int)]

    lib.digest_init.argtypes = [ctypes.POINTER(Context), ctypes.c_int]
    lib.digest_update.argtypes = [ctypes.POINTER(Context), ctypes.c_void_p, ctypes.c_size_t]
    lib.digest_update.restype = ctypes.c_bool
    lib.digest_final.argtypes = [ctypes.POINTER(Context), ctypes.c_void_p]
    for kind, algorithm in enumerate((hashlib.md5, hashlib.sha256)):
        for length in (0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 4095, 4096, 4097, 8193, 1048576):
            data = bytes(i % 251 for i in range(length))
            for chunk in (1, 17, 4096):
                ctx = Context()
                lib.digest_init(ctypes.byref(ctx), kind)
                for offset in range(0, length, chunk):
                    block = data[offset:offset+chunk]
                    assert lib.digest_update(ctypes.byref(ctx), block, len(block))
                out = (ctypes.c_uint8 * 32)()
                lib.digest_final(ctypes.byref(ctx), out)
                assert bytes(out)[:16 if kind == 0 else 32] == algorithm(data).digest()
        data = b"a" * 1000000
        ctx = Context()
        lib.digest_init(ctypes.byref(ctx), kind)
        assert lib.digest_update(ctypes.byref(ctx), data, len(data))
        out = (ctypes.c_uint8 * 32)()
        lib.digest_final(ctypes.byref(ctx), out)
        assert bytes(out)[:16 if kind == 0 else 32] == algorithm(data).digest()
    # Real 4 GiB generated stream, no disk file or whole-input allocation.
    # Both codecs cross the 32-bit byte-count boundary with a fixed 1 MiB chunk.
    block = bytes(range(256)) * 4096
    for kind, algorithm in enumerate((hashlib.md5, hashlib.sha256)):
        ctx = Context()
        lib.digest_init(ctypes.byref(ctx), kind)
        oracle = algorithm()
        for _ in range(4096):
            assert lib.digest_update(ctypes.byref(ctx), block, len(block))
            oracle.update(block)
        assert ctx.bytes == 1 << 32
        out = (ctypes.c_uint8 * 32)()
        lib.digest_final(ctypes.byref(ctx), out)
        assert bytes(out)[:16 if kind == 0 else 32] == oracle.digest()
    print("Independent digest vectors/boundaries and 4 GiB streams PASS (fixed context)")


if __name__ == "__main__":
    main()
