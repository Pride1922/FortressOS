# EXT4 Phase 0 delivery — 2026-10-03

Design/API/fixture tooling delivered. Runtime driver and physical acceptance remain pending.

Deliverables: [design baseline](../plans/EXT4_PHASE0.md), [overall plan](../plans/EXT4_PLAN.md), `src/fs/ext4.h`, `scripts/create_ext4_fixtures.py`, `make ext4-fixtures`.

Commands actually run:

```text
wsl -d Ubuntu-24.04 -- python3 scripts/create_ext4_fixtures.py
wsl -d Ubuntu-24.04 -- make ext4-fixtures
wsl -d Ubuntu-24.04 -- gcc -std=c11 -ffreestanding -Wall -Wextra -Werror -Isrc/include -Isrc/drivers -Isrc/fs -x c -fsyntax-only src/fs/ext4.h
git diff --check
```

Both fixture runs passed all three block sizes (1024/2048/4096): exact masks 0/0x42/0x403, independent Linux 1 MiB payload round trip and clean e2fsck -fn. Header syntax passed; diff check reported no whitespace errors (Git emitted a Makefile line-ending normalization notice).

Latest retained manifest: `build/ext4-phase0/fixtures-5xhop1gl/manifest.json`; earlier independent run: `build/ext4-phase0/fixtures-qu53f43b/manifest.json`. Generated images are disposable regular files, not production disks. Each manifest includes tool version, exact formatting command, UUID and image/payload hashes.

No FortressOS ext4 read/write, unsupported-feature admission, checksum engine, 4096-sector transport, crash recovery, QEMU or physical pass is implied. Malformed fixture checksums are intentionally invalid; later valid-checksum unsupported-feature fixtures must isolate mask rejection. The crash simulator is specified, not implemented. Existing storage/USB/DMA behavior was not changed.
