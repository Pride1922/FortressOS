# Download tools: checksums, tar, and traceroute

Status: DRAFT FOR USER REVIEW (2026-10-03). This document proposes three milestones; no implementation or new network ABI is approved by drafting it. Wget is implemented, but Dell acceptance remains pending.

## Order and boundaries

Implement checksums first, tar second, traceroute third. Checksums provide an independent download/extraction gate. Traceroute has a separate kernel design gate because existing ping cannot vary TTL or report ICMP errors.

Existing `/bin/ping` remains the reachability tool. Neither `tracert` nor `traceroute` currently exists. Preserve existing socket and NETCTL_PING ABIs, BSP ownership, worker cadence, wait signatures, lock ranks, driver workarounds and DMA quarantine. No scheduler, signal or timer-hook changes are proposed.

All tools are freestanding, stream bounded buffers, handle short I/O, close owned descriptors and return nonzero on failure. Package binaries explicitly in the initramfs. Host sanitizer tests prove logic; QEMU proves live integration; physical results are recorded separately.

## Milestone 1 — md5sum and sha256sum

### A. Freeze the userspace contract

Both tools process files in bounded chunks; they must never load a whole file into memory. Hashing a 4 GiB input must use the same fixed buffer and digest context as a small file, verified with a generated host input stream independently of guest filesystem capacity.

- `sha256sum [FILE ...]` and `md5sum [FILE ...]`; no operands or `-` reads stdin.
- `sha256sum -c MANIFEST` and `md5sum -c MANIFEST` verify files.
- Default output: lowercase digest, two spaces, filename, newline; stdin is named `-`.
- Verification accepts the usual digest followed by a space and either a space or `*` marker. Both modes hash exact bytes, with no newline conversion.
- Verification prints `FILE: OK` or `FILE: FAILED`; diagnostics go to stderr. Continue independent entries, but any mismatch, malformed entry or I/O failure makes the aggregate status 1. Usage errors return 2.
- Bound manifest lines to 512 bytes and paths to the existing filesystem limit. Explicitly reject unsupported escaped/newline-containing filenames rather than generating ambiguous manifests. Never silently skip malformed entries.

SHA-256 is the normal download workflow. MD5 is for compatibility with existing manifests, not a security recommendation. A manifest fetched from the same untrusted source does not establish authenticity.

### B. Implement and verify

Separate incremental digest codecs from CLI/syscall adapters. Use a fixed streaming buffer, byte-wise endian access and checked length accounting; no whole-file allocation or SIMD.

Host gates: literal known-answer vectors, empty/binary inputs, block-boundary lengths, million-byte and multi-buffer inputs, independent Python/Linux digest comparison, short reads/writes, failures and descriptor cleanup. Manifest tests cover bad hex, overlong/truncated lines, missing files and mixed success/failure.

QEMU gates: BIOS/UEFI, file/stdin/pipeline hashing, valid and invalid manifests, exact exit statuses and shell recovery. Test on disposable writable storage only.

Commit boundary: codecs, both tools, packaging, tests and documented CLI.

## Milestone 2 — safe, deliberately small tar extraction

### A. Freeze archive and destination policy

- `tar -tf ARCHIVE` lists; `tar -xf ARCHIVE -C NEW_DIRECTORY` extracts.
- MVP accepts uncompressed POSIX USTAR regular files and directories only.
- Reject symlinks, hard links, devices, FIFOs, sparse files, PAX/GNU extensions and compressed archives with explicit diagnostics. No archive creation or metadata restoration in this milestone.
- Extraction requires a destination that does not exist. Create it using the existing mkdir operation; never intentionally overwrite an existing destination tree.
- A failed extraction may leave a partial destination tree that the user must clean up. The tool identifies the retained directory but does not delete it, because another process may have modified it.
- Reject absolute paths, parent traversal, duplicate canonical member paths and file/directory prefix conflicts. Validate USTAR prefix/name together against actual filesystem bounds: full path less than 256 bytes, each component less than 64 bytes.
- Proposed reviewable caps: 256 members, depth 16, 16 MiB per file, 64 MiB total expanded data, 4096-byte streaming buffer. Keep tables off the small userspace stack and measure memory use.

The filesystem has no O_EXCL or seek syscall, and rename can replace its destination. This MVP therefore requires that no other process modifies the archive or extraction directory concurrently. It does not promise race-proof no-clobber extraction. Stronger guarantees require a separate filesystem/API design review.

### B. Validate before mutation, then extract

Pass 1 streams and validates the complete archive before creating the destination: header checksums, USTAR magic/version, bounded octal fields, size/padding arithmetic, supported types, paths, member conflicts, data completeness and end markers. Require two zero end blocks and only zero padding thereafter.

Reopen the archive for pass 2 because there is no seek syscall. Extract by bounded streaming. Compare a streaming SHA-256 over both passes to detect source changes; this detects a changed archive but is not an atomic snapshot guarantee.

**Open review issue — changed-archive guarantee:** the requested rule, "abort before creating the destination if the pass-2 hash differs," cannot be enforced by this two-pass streaming design: the full pass-2 hash becomes available only after extraction. An additional preflight hash pass could reject changes observed before extraction, but cannot prevent changes during the subsequent extraction pass. A strict guarantee of no partial extraction from a changed archive requires a stable snapshot or enforced source immutability, which this plan has not established. Resolve this requirement before tar implementation; do not claim that the final hash comparison provides it.

On any extraction/write/source-change failure, return nonzero and identify the retained partial destination. Do not recursively delete a tree that another process could have modified. Success does not promise power-failure durability; document `sync` before shutdown/removal.

### C. Verification

Host gates: independent USTAR fixtures, exact extracted binary bytes, nested directories, every supported size boundary, malformed headers/numbers, traversal, duplicate/prefix conflicts, unsupported types, truncation, source changes and injected I/O failures. Verify invalid archives cause no destination creation.

QEMU gates: list and extract an independently generated archive, compare tree and bytes, reject extraction into an existing destination, preserve unrelated files, verify failure status and shell recovery. Cleanly shut down disposable ext2 fixtures, inspect them independently and run `e2fsck -fn`.

Commit boundary: scanner/extractor, CLI, tests, packaging and limitations.

## Shared Dell gate — wget plus verification and extraction

Use a controlled LAN HTTP server with a known USTAR archive and independently recorded SHA-256 manifest. Wget currently supports plain HTTP; do not make HTTPS a requirement for this gate.

1. Download archive and manifest to `/mnt`; verify SHA-256.
2. List the archive and extract into a fresh directory; compare known binary/text contents.
3. Modify a downloaded byte and prove verification fails.
4. Exercise wget redirect, missing resource and truncated/interrupted download; partial downloads must not be treated as verified archives.
5. Confirm prompt and ping recovery, then sync, shut down cleanly, reboot and verify persisted files.

Record commands, fixture version, peer logs, expected digests and screen evidence. Existing wget host/QEMU evidence remains separate from this pending hardware gate.

## Milestone 3 — finite ICMP traceroute

### A. Design review before kernel code

Implement `/bin/traceroute`; an optional `tracert` alias can reuse it. Numeric IPv4 destinations only initially. Existing ping stays unchanged.

Current ping sends TTL 64 and exposes echo outcomes only. A real traceroute needs variable TTL and ICMP Time Exceeded/Destination Unreachable delivery. Draft `NETCTL_TRACE_ABI.md` first, with a new command, concrete sizes/offsets, validation/error precedence, result publication and owner/wait lifecycle. The user owns this approval gate. Do not repurpose ping's reserved fields.

Proposed limits: TTL 1–30, three probes per hop, one-second default timeout (maximum five), one outstanding trace probe, and a 120-second whole-command budget including ARP. Print router address/RTT or `*`; stop on matching destination Echo Reply, terminal unreachable or budget expiry. Document BSP tick RTT resolution.

Specify ping/trace contention and bounded cleanup explicitly: a busy shared facility returns EBUSY rather than cancelling another caller. Reuse established worker and wait mechanisms; STOP, interruption and owner exit must not leave a permanent reservation.

### B. Bounded ICMP error parsing and matching

Validate outer IPv4/ICMP checksums and bounds. Parse the quoted IPv4 header plus the first eight payload bytes independently: quotes need not include the entire original datagram. Match protocol, source/destination and echo identifier/sequence with a documented stale-probe/reuse guard. Do not assume the quote includes the payload token.

Handle Time Exceeded TTL expiry and explicitly enumerated Destination Unreachable codes. Reject truncated, fragmented, unrelated, stale and duplicate quotes without publishing a result. Router source addresses need not equal the destination. Matching is correlation, not authentication.

### C. Executable and physical gates

Host tests: independent quoted-wire vectors, TTL boundaries, checksum/length failures, wrong tuples, delayed replies, duplicates, contention, deadlines and owner cleanup.

QEMU: controlled synthetic router fixture producing multiple hops, timeout and unreachable outcomes; independently audit outbound TTL progression and quoted replies. SLIRP alone is insufficient evidence for multiple-hop behavior. Run existing ping/UDP/TCP/DNS and link/configuration regression fences.

Dell: direct LAN trace, nonresponding target, clean interruption and follow-up ping. A real multiple-hop claim requires a controlled routed topology or capture at the router/mirror point; a peer Wi-Fi capture alone cannot prove all intermediate hops. Leave that gate pending if the topology is unavailable.

Commit boundary: approved ABI/proof, ICMP error handling, tool, independent fixtures and documented evidence.

## Stop conditions and review decisions

Stop for discussion if archive safety needs a new filesystem primitive, traceroute requires a frozen ABI change, or implementation requires scheduler/signal/lock/timer/driver changes. Missing physical evidence remains pending rather than being inferred from QEMU.

Review requested: uncompressed USTAR/new-directory policy and caps; both digest tools with SHA-256 as default; finite numeric ICMP traceroute with a separate ABI approval gate. Each milestone lands independently after its executable gates pass.

## Primary references

- [RFC 1321 — MD5](https://www.rfc-editor.org/info/rfc1321/)
- [NIST FIPS 180-4 — SHA-256](https://csrc.nist.gov/pubs/fips/180-4/upd1/final)
- [RFC 792 — ICMP errors and quoted datagrams](https://www.rfc-editor.org/info/rfc792/)
- [Wget plan](WGET_PLAN.md), [network subsystem](../subsystems/net.md), [VFS API](../../src/fs/vfs.h), [ping ABI](../../src/include/ping_abi.h).
