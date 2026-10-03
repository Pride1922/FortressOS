# Download tools milestone 1 — streaming checksums

2026-10-03. `/bin/md5sum` and `/bin/sha256sum` are implemented and packaged in the normal initramfs. These checksum tools leave kernel, filesystem and network ABIs unchanged. [Traceroute is also implemented](net-traceroute.md); tar's changed-archive policy remains open in [the plan](../plans/DOWNLOAD_TOOLS_PLAN.md).

## Implementation

Actual freestanding incremental codecs live in `user/tools/digest.c`; the shared CLI in `user/tools/checksum.c` uses existing stream-tool syscall/output helpers. Data reads are capped at 4096 bytes; verification has a separate fixed 4096-byte manifest buffer and 512-byte line limit. Digest contexts and compression state are fixed-size. Input length never determines allocation; checked 64-bit byte accounting rejects bit-length overflow. No heap allocation, SIMD or whole-file buffering.

Both tools support file operands, stdin/no operands, `--`, `--help` and `-c MANIFEST`. Verification supports ordinary text/binary markers and CRLF or an unterminated final line, hashes exact bytes, reports each result and preserves an aggregate nonzero status. Unsupported escaped filenames/stdin manifest entries, invalid entries and empty manifests fail explicitly. Short I/O, read interruption, broken output, zero writes, open and close failures propagate with owned-descriptor cleanup. See [command reference](../../COMMANDS.md#md5sum--sha256sum).

## Executed gates

- `make -j4`: freestanding ELF/initramfs/ISO/raw-image build PASS; raw image's normal GPT/FAT/ext2 verification PASS. This is build evidence, not hardware execution.
- `make test-checksum-host`: actual CLI plus codec literal vectors and syscall-adapter failures/ownership under ASan/UBSan PASS. Independent Python hashlib comparisons cover binary/block boundaries, different chunking and million-byte vectors. Generated 4 GiB streams exercise each actual codec across the 32-bit byte-count boundary without a large file/allocation. Independent ctypes codec comparisons use a normal optimized shared library; the sanitizer executable covers actual CLI/codecs with adapters. These tests do not claim a guest filesystem can store a 4 GiB file.
- `python3 scripts/test_checksum.py`: BIOS 16/16 and UEFI 16/16 cases PASS, SMP=1, real Ring 3 tools, disposable ISO/OVMF, no data disks/network. Exact digest/status lines are compared to Python hashlib values, with explicit shell exit-status checks. Covers binary file, pipeline/stdin, empty file, valid/wrong/malformed manifests, missing-plus-valid operands and usage errors. Both guests power off cleanly.
- `make test-stream-tools-host`: existing actual stream-tool host regression PASS.

QEMU artifacts are retained under `build/checksum-qemu/20261003-090514-647128/`: fixture ISO/root, firmware variables, exact argv, UART, stderr and per-case expected results. The runner rejects extra storage arguments and bounds waits, drains UART independently and reaps QEMU on failure. Additional final verification runs are retained in timestamped sibling directories.

Dell checksum/wget physical acceptance is pending. No physical, general cryptographic audit, power-failure durability or writable-filesystem claim follows from the no-data-disk QEMU gate. MD5 is compatibility-only; prefer SHA-256 and independently trusted expected digests.
