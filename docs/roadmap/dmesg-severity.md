# dmesg Severity Level Filtering — Contract and Verification Evidence

## 1. Overview and Invariants

This document records the implementation contracts and verification evidence for severity level filtering in FortressOS `dmesg` (both the shell builtin and `/bin/dmesg`).

### Invariants
1. **RFC 5424 Severity Spectrum**:
   - `0` = `KLOG_EMERG` (emerg, panic, emergency)
   - `1` = `KLOG_ALERT` (alert)
   - `2` = `KLOG_CRIT` (crit, critical)
   - `3` = `KLOG_ERR` (err, error)
   - `4` = `KLOG_WARNING` (warn, warning)
   - `5` = `KLOG_NOTICE` (notice)
   - `6` = `KLOG_INFO` (info) — default log level
   - `7` = `KLOG_DEBUG` (debug)
2. **Ring Buffer Entry Capture**:
   - The kernel log ring buffer (`DMESG_SIZE` = 64 KiB) retains severity levels for all lines using prefix markers `<N>`.
   - The ring buffer entry struct `dmesg_entry_t` in `src/kernel/dmesg.h` records `uint8_t level` and `uint16_t len`.
   - Logging macros `pr_emerg()`, `pr_alert()`, `pr_crit()`, `pr_err()`, `pr_warn()`, `pr_notice()`, `pr_info()`, `pr_debug()` and `klog(level, str)` publish directly to the ring buffer.
   - Physical output to UART (COM1) and framebuffer strips `<N>` prefixes so raw console displays clean human text.
3. **Userland CLI Specification**:
   - Accepts `-l <levels>` and `--level=<levels>`.
   - Accepts comma-separated level names and numeric values `0`–`7`.
   - Bounded parsing: invalid level names or out-of-range numbers reject with exit code `2` and clear diagnostic.
   - Combined filtering evaluates `(Facility matches) AND (Level matches)`.
   - Standard output strips `<N>` prefixes unless raw mode (`-r`, `--raw`) is requested.
   - Zero dynamic allocation: parsing and compaction occur in-place in static buffer `s_dmesg_buf`.

## 2. Verification Evidence

### Host ASan/UBSan Unit Tests (`make test-dmesg-filter-host`)
- Filter parsing (`-f NET`, `-f=NET`, `-fNET`, `--facility=NET`, `--facility NET`)
- Case-insensitive facility matching and exact boundary checks (`[NET]` vs `[NETWORK]`)
- Multi-facility union (`-f NET -f USB`)
- Unfiltered preservation
- Nonexistent facility handling (empty output, exit code 0)
- Severity level parsing (`-l err`, `-l err,crit`, `-l 3`, `-l 2,3`, `-l 0..7`)
- Combined facility and level intersection (`-f TEST -l warn`)
- Raw mode (`-r` / `--raw`) retaining `<N>`
- Bounded error handling: invalid names (`-l invalid`), numbers (`-l 8`), missing args, empty strings, trailing commas, all rejecting with exit status 2
- 100% PASS under Clang/GCC ASan/UBSan.

### Multi-Firmware QEMU Integration Suite (`make test-dmesg` / `scripts/test_dmesg_filter.py`)
- Verified on Legacy BIOS at SMP=1.
- Verified on UEFI (OVMF) at SMP=1.
- Verified `dmesg -l err` returns exactly `[TEST] test log level 3 (err)`.
- Verified `dmesg -l err,crit` returns levels 2 and 3.
- Verified `dmesg -f TEST -l warn` correctly intersects facility `TEST` and level `warn`.
- Verified `dmesg -l 3` numeric filtering.
- Verified `dmesg -l invalid` exits with status 2.
- Verified regression safety on existing facility filters (`-f NET`, `-f USB`, `-f PCI`), tail capping (`-n 5`), persistent file output (`/mnt/net.log`), and standalone ELF `/bin/dmesg`.
