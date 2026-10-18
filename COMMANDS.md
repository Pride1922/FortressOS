# FortressOS Command Reference

A comprehensive guide to every command, shell builtin, core utility, and diagnostic tool in FortressOS.

---

## Table of Contents

1. [Architecture & Execution Model](#1-architecture--execution-model)
2. [Quick Reference Index](#2-quick-reference-index)
3. [Shell Built-in Commands](#3-shell-built-in-commands)
4. [Line Editor (`edit`) Subcommands](#4-line-editor-edit-subcommands)
5. [Core Utilities & Stream Tools](#5-core-utilities--stream-tools)
6. [System & Process Introspection](#6-system--process-introspection)
7. [Network Management & Internet Tools](#7-network-management--internet-tools)
8. [Diagnostic, Test & Internal Binaries](#8-diagnostic-test--internal-binaries)
9. [Keyboard Shortcuts & Line Editing](#9-keyboard-shortcuts--line-editing)

---

## 1. Architecture & Execution Model

FortressOS executes user programs in **Ring 3** with hardware memory protection, distinct page directories, and System V AMD64 ABI conventions.

* **Shell Environment**: The interactive shell (`user/shell/`) provides a command-line interface with history, job control, pipeline orchestration, and path resolution.
* **Builtin Commands**: Handled directly by the parent shell process. State-altering builtins (such as `cd`, `export`, `alias`, `jobs`, `fg`, `bg`) execute in the shell's own context.
* **Child-Safe Builtins in Pipelines**: Builtins that only transform or emit data (such as `echo`, `pwd`, `env`, `ls`, `view`, `type`, `true`, `false`, `version`, `help`) are marked *child-safe*. When placed in a pipeline (e.g. `echo hello | wc -c`), the shell spawns `/bin/sh-builtin` to execute them safely as isolated pipeline stages without polluting parent state.
* **External Binaries**: Standalone ELF executables stored in `/bin/` on the initial RAM filesystem (`initramfs.tar`). When an unknown command name without a slash is entered, the shell automatically searches `/bin/` (or the directories specified in `$PATH`).
* **Filesystem Paths**: The root filesystem (`/`) is an in-memory TarFS archive (read-only). Writable and persistent files live under `/mnt` (the mounted ext2 storage partition on NVMe or USB).

### Shell scripts

The scripting worktree adds `sh file [args...]`, `sh -c command [name [args...]]`
and command input from nonterminal stdin. It supports variables, `$?`, `$0`,
positional arguments, `$#`, quoted `"$@"`, `shift [count]`, existing simple
pipelines/redirections, `if`/`elif`/`else`, `while`, `for`, and
`break [levels]` / `continue [levels]`, `source file [args...]` / `. file [args...]`,
and `return [status]` inside sourced files.

```sh
sh -c 'for item in one "two words"; do echo "$item"; done'
sh -c 'if [ -d /mnt ]; then echo "Mount path exists"; fi'
```

`test` and `[` support fixed-arity string, file and signed-integer predicates;
`[` requires a closing `]`. Expression errors return 2, false returns 1, and
true returns 0. Use ordinary `&&`/`||` command lists for boolean composition.
Source shares variables/cwd, preserves `$0`, restores explicitly supplied
arguments afterward, and supports eight nested files. Without source arguments,
`shift` changes the caller’s parameters. `return` accepts 0..255 (default `$?`);
using it outside a sourced file is an error. Source paths are opened as given,
relative to cwd when needed; PATH lookup is not performed. Source/return are
parent-only and cannot run as pipeline stages or background commands.
Functions, arithmetic and substitutions are not implemented. Compounds
must be separated by newlines/semicolons and cannot themselves be pipeline
stages or carry trailing redirections. See [the scripting guide and bounds](docs/roadmap/shell-scripting.md).
This feature is isolated from the main checkout and is not in its last USB image.

---

## 2. Quick Reference Index

| Command | Type | Category | Summary |
|:---|:---|:---|:---|
| [`help`](#help) | Builtin (child-safe) | Shell | Show help overview or help for a specific command |
| [`clear`](#clear) | Builtin (child-safe) | Shell | Clear the screen and move the cursor home |
| [`cd`](#cd) | Builtin | Navigation | Change working directory |
| [`pwd`](#pwd) | Builtin (child-safe) | Navigation | Print working directory |
| [`type`](#type) | Builtin (child-safe) | Shell | Display how a command name would be interpreted |
| [`command`](#command) | Builtin | Shell | Execute simple command bypassing aliases |
| [`true`](#true) | Builtin (child-safe) | Shell | Return success exit status (0) |
| [`false`](#false) | Builtin (child-safe) | Shell | Return failure exit status (1) |
| [`ls`](#ls) | Builtin (child-safe) | Filesystem | List directory contents or file status |
| [`view`](#view) | Builtin (child-safe) | Filesystem | View text file with non-printable characters sanitized |
| [`edit`](#edit) | Builtin | Editor | Open interactive line-oriented text editor |
| [`mkdir`](#mkdir) | Builtin | Filesystem | Create a new directory |
| [`rm`](#rm) | Builtin | Filesystem | Remove a file or empty directory |
| [`mv`](#mv) | Builtin | Filesystem | Rename or move a file or directory |
| [`cp`](#cp) | Builtin / `/bin/cp` | Filesystem | Copy files and preserve permissions with bounded buffers |
| [`touch`](#touch) | Builtin / `/bin/touch` | Filesystem | Update file timestamps or create empty files |
| [`chmod`](#chmod) | Binary (`/bin/chmod`) | Filesystem | Change file or directory access permissions |
| [`chown`](#chown) | Binary (`/bin/chown`) | Filesystem | Change file or directory ownership and group |
| [`sync`](#sync) | Builtin | Filesystem | Flush dirty filesystem buffers to persistent storage |
| [`echo`](#echo) | Builtin (child-safe) | Text | Print arguments to standard output (supports `$?`, `$VAR`) |
| [`printf`](#printf) | Builtin (child-safe) | Text | Format and print data without trailing newline (exact bytes, RFC vectors) |
| [`run`](#run) | Builtin | Process | Compatibility wrapper to spawn an executable binary |
| [`layout`](#layout) | Builtin | System | Query or set keyboard layout (`us` or `azerty`) |
| [`reboot`](#reboot) | Builtin / `/sbin/reboot` | System | Save state and reboot system |
| [`shutdown`](#shutdown) / [`poweroff`](#poweroff) | Builtin / `/sbin/shutdown`, `/sbin/poweroff` | System | Save state and ACPI power off |
| [`exit`](#exit) | Builtin | Shell | Exit the current shell session |
| [`dmesg`](#dmesg) | Builtin / `/bin/dmesg` | System | Display, tail, or save kernel diagnostic ring buffer log |
| [`history`](#history) | Builtin | Shell | Display, clear, save, or load command history |
| [`prompt`](#prompt) | Builtin | Shell | Configure shell prompt template |
| [`terminal`](#terminal) | Builtin | Shell | Select terminal display mode (`local`, `serial`, `mirror`, `plain`) |
| [`set`](#set) | Builtin | Environment | Display all shell variables |
| [`unset`](#unset) | Builtin | Environment | Unset shell variables |
| [`export`](#export) | Builtin | Environment | Set or export environment variables to child processes |
| [`env`](#env) | Builtin / `/bin/env` | Environment | Set environment and run command, or print exported environment variables |
| [`which`](#which) | Binary (`/bin/which`) | Shell | Locate a command in PATH |
| [`alias`](#alias) | Builtin | Shell | Define or display command aliases |
| [`unalias`](#unalias) | Builtin | Shell | Remove command aliases |
| [`jobs`](#jobs) | Builtin | Job Control | List active background and stopped jobs |
| [`fg`](#fg) | Builtin | Job Control | Bring background or stopped job to foreground |
| [`bg`](#bg) | Builtin | Job Control | Resume stopped job in the background |
| [`kill`](#kill) | Builtin and binary (`/bin/kill`) | Job Control | Signal a positive PID or an owned shell job |
| [`version`](#version) | Builtin (child-safe) | System | Print FortressOS version and build information |
| [`cat`](#cat) | Binary (`/bin/cat`) | Stream Tool | Concatenate and print files with exact byte fidelity |
| [`head`](#head) | Binary (`/bin/head`) | Stream Tool | Output first part of files (lines or bytes) |
| [`tail`](#tail) | Binary (`/bin/tail`) | Stream Tool | Output last part of files (bounded buffer) |
| [`wc`](#wc) | Binary (`/bin/wc`) | Stream Tool | Print newline, word, and byte counts |
| [`grep`](#grep) | Binary (`/bin/grep`) | Stream Tool | Search text matching regular expressions with bounded bitmask NFA |
| [`sort`](#sort) | Binary (`/bin/sort`) | Stream Tool | Sort lines of text files with stack-free bottom-up merge sort |
| [`uniq`](#uniq) | Binary (`/bin/uniq`) | Stream Tool | Filter adjacent matching lines |
| [`xxd`](#xxd) | Binary (`/bin/xxd`) | Stream Tool | Make hex dump or revert hex to binary with bounded buffers |
| [`diff`](#diff) | Binary (`/bin/diff`) | Stream Tool | Compare files line by line using stack-free Myers algorithm |
| [`patch`](#patch) | Binary (`/bin/patch`) | Stream Tool | Apply unified or normal diff files with fail-closed transactional safety |
| [`sed`](#sed) | Binary (`/bin/sed`) | Stream Tool | Bounded stream editor with bitmask NFA regex and line addressing |
| [`awk`](#awk) | Binary (`/bin/awk`) | Stream Tool | Bounded pattern scanning and text processing language |
| [`diskbench`](#diskbench) | Binary (`/bin/diskbench`) | Stream Tool / Benchmark | Filesystem throughput and latency benchmark tool |
| [`disk`](#disk) | Binary (`/sbin/disk`) | Storage Tool | Storage device inspection and filesystem usage tool |
| [`df`](#df) | Binary (`/bin/df`) / Alias | Storage Tool | Display filesystem disk space and inode usage (alias for `disk usage`) |
| [`nano`](#nano) | Binary (`/bin/nano`) | Editor | Full-screen interactive visual text editor |
| [`date`](#date) | Binary (`/bin/date`) | Time / System | Display or format system date and time via `SYS_CLOCK_GETTIME` |
| [`sleep`](#sleep) | Binary (`/bin/sleep`) | Time / Process | Delay execution for specified duration via `SYS_NANOSLEEP` |
| [`gzip`](#gzip) | Binary (`/bin/gzip`) | Stream Tool / Compression | Bounded DEFLATE (RFC 1951) and gzip (RFC 1952) compression |
| [`gunzip`](#gunzip) | Binary (`/bin/gunzip`) | Stream Tool / Compression | Bounded DEFLATE (RFC 1951) and gzip (RFC 1952) decompression |
| [`ps`](#ps) | Binary (`/bin/ps`) | Introspection | Snapshot active process table |
| [`top`](#top) | Binary (`/bin/top`) | Introspection | Real-time interactive CPU & process monitor |
| [`sysinfo`](#sysinfo) | Binary (`/bin/sysinfo`) | Introspection | Display OS identity, CPU, memory/heap, tasks, and storage summary |
| [`free`](#free) | Binary (`/bin/free`) | Introspection | Display free and used memory summary from SYS_SYSINFO |
| [`ifconfig`](#ifconfig) | Binary (`/sbin/ifconfig`) | Networking | Query network interface status and packet counters |
| [`ifup`](#ifup) | Binary (`/sbin/ifup`) | Networking | Configure network interface statically or via config file |
| [`dhcpc`](#dhcpc) | Binary (`/sbin/dhcpc`) | Networking | Acquire dynamic IPv4 lease and configure network interface |
| [`ping`](#ping) | Binary (`/bin/ping`) | Networking | Send ICMP Echo Request packets to IPv4 host or hostname |
| [`traceroute`](#traceroute) | Binary (`/bin/traceroute`) | Networking | Finite numeric ICMP trace with TTL-expiry/error reporting |
| [`tracert`](#traceroute) | Alias (`traceroute`) | Networking | Alias for `traceroute` |
| [`nslookup`](#nslookup) | Binary (`/bin/nslookup`) | Networking | Query DNS name server for IPv4 addresses |
| [`nc`](#nc) | Binary (`/bin/nc`) | Networking | Arbitrary TCP connections and listens (Netcat) |
| [`wget`](#wget) | Binary (`/bin/wget`) | Networking | Download files over HTTP/1.0 and HTTP/1.1 |
| [`download`](#download) | Binary (`/bin/download`) | Networking | Simple file downloader (`download [dest] <URL>`) |
| [`md5sum` / `sha256sum`](#md5sum--sha256sum) | Binaries (`/bin/md5sum`, `/bin/sha256sum`) | Files | Stream digests and verify manifests |
| [`sudo`](#permissions-and-sudo) | Binary (`/bin/sudo`) [setuid root] | System / Security | Execute command with superuser (root) privileges |
| [`passwd`](#passwd) | Binary (`/bin/passwd`) [setuid root] | System / Security | Update login password for self or user |
| [`useradd`](#useradd) | Binary (`/sbin/useradd`) | System / Accounts | Provision persistent user account and create home directory |
| [`lockstat`](#lockstat) | Binary (`/sbin/lockstat`) | Diagnostic | Display kernel spinlock contention and hold-time metrics |
| [`hello`](#hello) | Binary (`/bin/hello`) | Diagnostic | Test ELF binary with argument echoing and `--spin` |
| [`dual_stream`](#dual_stream) | Binary (`/bin/dual_stream`) | Diagnostic | Diagnostic tool emitting distinct stdout and stderr streams |
| [`dhcp-probe`](#dhcp-probe) | Binary (`/bin/dhcp-probe`) | Diagnostic | Broadcast DHCPDISCOVER probe verifying end-to-end broadcast RX/TX |
| [`tcptest`](#tcptest) | Binary (`/bin/tcptest`) | Diagnostic | TCP client test fixture |
| [`tcpserve`](#tcpserve) | Binary (`/bin/tcpserve`) | Diagnostic | TCP echo server test fixture |
| [`udptest`](#udptest) | Binary (`/bin/udptest`) | Diagnostic | UDP client and listener test fixture |
| [`dnsprobe`](#dnsprobe) | Binary (`/bin/dnsprobe`) | Diagnostic | DNS deadline and signal test fixture |
| [`tcpdeadline`](#tcpdeadline) | Binary (`/bin/tcpdeadline`) | Diagnostic | TCP connect/send timeout test fixture |
| [`net-ping-probe`](#net-ping-probe) | Binary (`/bin/net-ping-probe`) | Diagnostic | Ring 3 NETCTL ping ABI probe |
| [`net-udp-probe`](#net-udp-probe) | Binary (`/bin/net-udp-probe`) | Diagnostic | Ring 3 UDP socket ABI probe |
| [`sh-builtin`](#sh-builtin) | Binary (`/bin/sh-builtin`) | System | Pipeline stage runner for child-safe builtins |
| [`init`](#init) | Binary (`/bin/init`) | System | Kernel init process and Ring 3 test harness |

---

## 3. Shell Built-in Commands

### `clear`
**Syntax:** `clear`

**Description:** Clears the visible terminal screen and moves the cursor to the top left. The shell prints a fresh prompt; command history is retained.

### `help`
**Syntax:** `help [command]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Displays the general help summary, listing all available built-in commands and editing shortcuts. If `command` is supplied, displays the specific summary for that command.  
**Examples:**
```sh
help
help ls
help | head -n 10
```

---

### `cd`
**Syntax:** `cd [path | -]`  
**Child-Safe in Pipelines:** No (modifies parent shell state)  
**Description:** Changes the working directory of the shell.
* `cd` with no arguments changes to the root directory `/`.
* `cd -` switches back to the previous working directory (`$OLDPWD`).
* Path can be absolute (e.g. `/mnt`) or relative (e.g. `bin`, `..`).  
**Examples:**
```sh
cd /mnt
cd ..
cd -
```

---

### `pwd`
**Syntax:** `pwd`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints the current working directory to standard output.  
**Examples:**
```sh
pwd
```

---

### `type`
**Syntax:** `type <name> ...`  
**Child-Safe in Pipelines:** Yes  
**Description:** Inspects each argument and prints how the shell would interpret it: as a shell builtin, an alias, or an external executable found via `$PATH`.  
**Examples:**
```sh
type cd
type ls
type wget
```

---

### `command`
**Syntax:** `command <cmd> [args...]`  
**Child-Safe in Pipelines:** No  
**Description:** Runs `<cmd>` directly, bypassing alias lookup. Useful when an alias has shadowed a built-in or binary.  
**Examples:**
```sh
command ls
```

---

### `true`
**Syntax:** `true`  
**Child-Safe in Pipelines:** Yes  
**Description:** Returns a successful exit status (`0`). Does nothing.  
**Examples:**
```sh
true && echo "Success"
```

---

### `false`
**Syntax:** `false`  
**Child-Safe in Pipelines:** Yes  
**Description:** Returns a failure exit status (`1`). Does nothing.  
**Examples:**
```sh
false || echo "Failed as expected"
```

---

### `ls`
**Syntax:** `ls [path]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Lists directory contents. If `path` is omitted, lists the current working directory (`.`). Directories are displayed with a trailing `/`. If `path` is a regular file, its name is printed.  
**Examples:**
```sh
ls
ls /bin
ls /mnt
```

---

### `view`
**Syntax:** `view [path]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Safe text viewer. Opens and displays a text file, replacing non-printable/control bytes with dots (`.`) and ensuring a final newline. If no `path` is given, reads from standard input.  
*Difference from `cat`:* `cat` outputs exact raw bytes without alteration; `view` sanitizes control characters to protect the terminal.  
**Examples:**
```sh
view /etc/motd
view /docs/readme.txt
echo "Binary \x01\x02 test" | view
```

---

### `edit`
**Syntax:** `edit <path>`  
**Child-Safe in Pipelines:** No (interactive)  
**Description:** Opens the built-in interactive line-oriented text editor for creating or modifying files up to 64 lines and 8192 bytes. See [Line Editor Subcommands](#4-line-editor-edit-subcommands) below for the command set.  
**Examples:**
```sh
edit /mnt/test.txt
edit /mnt/.fortress/network.conf
```

---

### `mkdir`
**Syntax:** `mkdir <path>`  
**Child-Safe in Pipelines:** No  
**Description:** Creates a new directory at `<path>` with default permissions (`0755`). Returns `0` on success or an error if the parent directory does not exist or storage is read-only.  
**Examples:**
```sh
mkdir /mnt/data
mkdir /mnt/downloads
```

---

### `rm`
**Syntax:** `rm <path>`  
**Child-Safe in Pipelines:** No  
**Description:** Removes a file or an empty directory at `<path>`. If the directory is non-empty, returns error `rm: directory not empty`.  
**Examples:**
```sh
rm /mnt/old.txt
rm /mnt/data
```

---

### `mv`
**Syntax:** `mv <old_path> <new_path>`  
**Child-Safe in Pipelines:** No  
**Description:** Renames or moves a file or directory from `<old_path>` to `<new_path>`.  
**Examples:**
```sh
mv /mnt/old.txt /mnt/new.txt
```

---

### `cp`
**Syntax:** `cp <source> <dest>` or `cp <source>... <directory>`  
**Path:** Builtin / `/bin/cp`  
**Child-Safe in Pipelines:** No  
**Description:** Copies files using a bounded-buffer read/write loop with exact byte fidelity. Preserves source file permissions upon successful copy. When the target destination is an existing directory, copies each source file into that directory with its original basename. Rejects copying a file onto itself or copying directories without recursion.  
**Examples:**
```sh
cp /etc/motd /mnt/motd.backup
cp /mnt/f1.txt /mnt/f2.txt /mnt/backup_dir
```

---

### `touch`
**Syntax:** `touch [-c] <path>...`  
**Path:** Builtin / `/bin/touch`  
**Child-Safe in Pipelines:** No  
**Description:** Updates timestamps or creates files. Opens files with `O_CREAT | O_WRONLY` without truncation, ensuring existing file contents are preserved intact. When `-c` or `--no-create` is supplied, non-existent files are not created.  
**Options:**
* `-c`, `--no-create`: Do not create missing files.
* `--`: Ends option scanning.  
**Examples:**
```sh
touch /mnt/newfile.txt
touch -c /mnt/maybe_exists.txt
touch file1.txt file2.txt file3.txt
```

---

### `sync`
**Syntax:** `sync`  
**Child-Safe in Pipelines:** No  
**Description:** Flushes all cached filesystem metadata and dirty data blocks to underlying persistent storage devices (NVMe and USB BOT drives).  
**Examples:**
```sh
sync
```

---

### `echo`
**Syntax:** `echo [-n] [-e] [-E] [args...]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints arguments separated by single spaces, followed by a newline (unless `-n` is specified). Evaluates variables expanded by the shell (e.g. `$?`, `$VAR`, `${VAR}`).
* `-n`: Do not print the trailing newline.
* `-e`: Enable interpretation of backslash escapes (`\\`, `\a`, `\b`, `\c`, `\e`, `\f`, `\n`, `\r`, `\t`, `\v`, `\0NNN` octal bytes, `\xHH` hex bytes). `\c` suppresses any further output immediately without a trailing newline.
* `-E`: Disable backslash escape interpretation (default).  
**Examples:**
```sh
echo "Hello, FortressOS!"
echo -n "No trailing newline"
echo -e "First line\nSecond line\t(tabbed)"
echo -e "Byte \x41 and octal \0102"      # Outputs Byte A and octal B
echo "Exit status:" $?
echo "Current path:" $PATH
```

---

### `printf`
**Syntax:** `printf <format> [arguments...]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Formats and writes text to standard output without an automatic trailing newline. Fills a vital pipeline gap where exact byte sequences are required (e.g. RFC test vectors, binary streams, or hashes).
* **Exact Bytes:** Does not append a newline unless explicitly requested via `\n` in the format string.
* **Format Specifiers:** Supports `%s` (string), `%b` (string with backslash escapes expanded, including `\0NNN` octal, `\xHH` hex, and width/precision padding), `%c` (single character), `%d` / `%i` (signed decimal integer), `%u` (unsigned decimal integer), `%x` / `%X` (hexadecimal), `%o` (octal), and `%%` (literal percent).
* **Width & Alignment:** Supports field widths (e.g. `%10s`, `%10b`), left-alignment (`%-10s`, `%-10b`), and zero-padding (`%04x`).
* **Escape Sequences:** Interprets `\n` (newline), `\t` (tab), `\r` (carriage return), `\xHH` (hex byte), `\0NNN` / `\NNN` (octal byte), `\e` (escape), and `\c` (terminate output immediately).
* **Format Reuse:** If more arguments are provided than format specifiers, the format string is reused until all arguments are consumed.

**Examples:**
```sh
printf "abc"                         # Exact 3 bytes 'abc' without trailing newline (RFC test vector)
printf "abc" | wc -c                 # Outputs 3 (contrasting with echo abc | wc -c which outputs 4)
printf "%s\n" "hello"                # Outputs 'hello\n' with explicit newline
printf "%s\n" foo bar baz            # Format reuse: outputs foo\nbar\nbaz\n
printf "count=%d hex=0x%04x\n" 42 42 # Outputs count=42 hex=0x002a\n
printf "a\tb\nc\n"                   # Tab and newline escape sequences
printf "\x41\x42\x43"                # Hexadecimal byte sequence (emits ABC)
```

---

### `run`
**Syntax:** `run <binary_path> [args...]`  
**Child-Safe in Pipelines:** No  
**Description:** Compatibility wrapper to execute an external ELF program. (Note: direct execution like `/bin/hello` or `hello` is also supported).  
**Examples:**
```sh
run /bin/hello
run /bin/sysinfo
```

---

### `layout`
**Syntax:** `layout [us | azerty]`  
**Child-Safe in Pipelines:** No  
**Description:** Queries or configures the active keyboard layout for the PS/2 keyboard driver.
* `layout` without arguments displays the current active layout.
* `layout us` switches to US QWERTY.
* `layout azerty` switches to Belgian AZERTY (with full AltGr and shifted punctuation decoding).  
**Examples:**
```sh
layout
layout azerty
layout us
```

---

### `reboot`
**Syntax:** `reboot`  
**Child-Safe in Pipelines:** No  
**Description:** Saves command history to `/mnt/.fortress/history`, terminates running jobs, flushes filesystem buffers, and reboots the machine via ACPI reset / keyboard controller reset.  
**Examples:**
```sh
reboot
```

---

### `shutdown` / `poweroff`
**Syntax:** `shutdown` or `poweroff`  
**Child-Safe in Pipelines:** No  
**Description:** Saves command history, cleanly shuts down active jobs, flushes disks, and powers off the machine via ACPI S5 sleep state.  
**Examples:**
```sh
shutdown
poweroff
```

---

### `exit`
**Syntax:** `exit [status]`  
**Child-Safe in Pipelines:** No  
**Description:** Saves command history, cleans up active jobs, and terminates the interactive shell session with the specified status code (or the exit status of the last executed command).  
**Examples:**
```sh
exit
exit 0
```

---

### `dmesg`
**Syntax:** `dmesg [-f <facility> | --facility=<facility>] [-a] [-n <lines>] [tail [<lines>]] [<lines>] [path]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints or saves kernel diagnostic ring buffer messages. Supports inspecting the most recent log entries directly, filtering by subsystem facility tag, or redirecting through pipelines.
* `dmesg` without arguments prints the complete kernel log to the terminal (or pipeline).
* `dmesg -f <FACILITY>` or `dmesg --facility=<FACILITY>` filters output to lines whose facility tag matches (e.g. `[NET]`, `[EXT4]`, `[USB]`). Filtering is case-insensitive (e.g. `-f net` matches `[NET]`) and exact against the tag name (matches `[NET]`, not `[NETWORK]`). Lines must start with `[` followed by the facility name and `]` (after optional leading whitespace).
* Multiple `-f` flags combine with OR logic (e.g. `dmesg -f NET -f USB` prints both network and USB messages). Up to 8 facility filters can be specified.
* `dmesg -n <N>` or `dmesg tail <N>` or `dmesg <N>` prints only the last `<N>` lines of the (optionally filtered) kernel log.
* `dmesg tail` defaults to the last 10 lines of the kernel log.
* `dmesg <path>` or `dmesg -f <FACILITY> <path>` or `dmesg -n <N> <path>` saves output to the specified file path (overwriting existing content).
* `dmesg -a <path>` or `dmesg --append <path>` appends output to the specified file path on disk without truncating.
* `dmesg` can run in pipelines (e.g. `dmesg | tail`, `dmesg | wc -l`). Also available as `/bin/dmesg`.

#### Troubleshooting with `dmesg` (The Killer Troubleshooting Feature)
Kernel subsystems—including USB storage (`[USB]`), network devices (`[NET]`), filesystem mutations (`[EXT4]`, `[JBD2]`), memory management (`[PMM]`, `[VMM]`), and SMP synchronization—write status, warnings, and metric counters continuously to the kernel diagnostic ring buffer.

Because FortressOS defaults to a quiet graphical splash with a clean shell handoff, the boot log can contain hundreds of initialization lines. Tailing `dmesg` is the single fastest way to troubleshoot runtime issues without drowning in early boot logs:

1. **Diagnosing USB Storage & Sync Performance:**
   When running disk-heavy commands or benchmarking sync times, the kernel appends performance rows (TSC cycle counts, BOT transfer counts, cache flush latencies) to `dmesg`. Running:
   ```sh
   sync
   dmesg -n 6
   ```
   immediately displays the latest sync rows and transaction commits without touching the rest of the screen.

2. **Network Connection & Packet Drop Troubleshooting:**
   If `wget` or `ping` encounters unexpected latency, timeouts, or connection failures, `dmesg tail` reveals interface link status transitions, socket states, and packet drop reasons:
   ```sh
   wget http://192.168.0.153:8000/testfile.txt
   dmesg tail 15
   ```

3. **Driver Bring-Up & Mount Issues:**
   If a partition fails to mount or an external device behaves unexpectedly:
   ```sh
   dmesg -n 25
   ```
   shows the exact SCSI sense codes, partition table parsing alerts, or filesystem mount rejections.

4. **Saving Post-Mortem Diagnostics:**
   To capture the exact failure state for reporting or offline analysis without overwriting the terminal:
   ```sh
   dmesg -n 30 /mnt/troubleshoot.log
   ```

**Examples:**
```sh
dmesg                           # Full kernel ring buffer log
dmesg -f NET                    # Only [NET] subsystem lines
dmesg -f EXT4                   # Only [EXT4] filesystem lines
dmesg -f NET -n 20              # Last 20 [NET] lines
dmesg -f NET -f USB             # Both [NET] and [USB] lines (OR filter)
dmesg -f NET /mnt/net.log       # Write [NET] lines to persistent disk file
dmesg --facility=NET            # Long form facility filter
dmesg --help                    # Show usage and list supported facilities
dmesg -n 13                     # Last 13 lines (instant recent troubleshooting)
dmesg tail 20                   # Last 20 lines using explicit 'tail' verb
dmesg 15                        # Last 15 lines using shorthand number
dmesg tail                      # Last 10 lines (default tail)
dmesg | tail                    # Piped into stream tool
dmesg | wc -l                   # Count total diagnostic lines emitted
dmesg /mnt/boot.log             # Dump entire ring buffer to persistent disk
dmesg -n 25 /mnt/error_tail.log # Save last 25 diagnostic lines to disk
```

---

### `history`
**Syntax:** `history [-c | clear | save | load]`  
**Child-Safe in Pipelines:** No  
**Description:** Manages the shell command history.
* `history` without arguments displays the numbered list of previous commands.
* `history clear` or `history -c` wipes the in-memory history buffer and immediately saves the cleared empty history to persistent storage (`/mnt/.fortress/history`).
* `history save` explicitly writes the history buffer to `/mnt/.fortress/history` and resets the auto-flush counter.
* `history load` reloads previous command history from `/mnt/.fortress/history`, resetting the auto-flush counter, dirty flag, and warning flag.

**Auto-Flush Behavior:**
During interactive sessions (when stdin is a terminal), history automatically auto-flushes to `/mnt/.fortress/history` every 5 accepted commands (`HISTORY_AUTOFLUSH_INTERVAL = 5`) whenever persistent storage is writable and history has changed.
* Triggers immediately after command completion so the command that just executed is included in the save.
* Failed commands (syntax errors, commands not found, non-zero exits) count toward the 5-command interval.
* On write failure (e.g. read-only filesystem), warns once per session and suppresses repeated warnings.
* Non-interactive execution (pipes and scripts) bypasses auto-flush and relies on clean-exit saves.  
**Examples:**
```sh
history
history save
history clear
history -c
```

---

### `prompt`
**Syntax:** `prompt [default | cwd | <template>]`  
**Child-Safe in Pipelines:** No  
**Description:** Configures the shell prompt appearance.
* `prompt` displays the current prompt template.
* `prompt default` resets the prompt to `fortress:<cwd> $ `, showing the current working directory.
* `prompt cwd` sets the prompt to `fortress:<cwd> $ `.
* `prompt "<template>"` sets a custom prompt string. `<cwd>` is dynamically replaced with the current directory.  
**Examples:**
```sh
prompt cwd
prompt "my-os: <cwd> # "
prompt default
```

---

### `terminal`
**Syntax:** `terminal [local | serial | mirror | plain]`  
**Child-Safe in Pipelines:** No  
**Description:** Queries or configures the terminal output mode via `SYS_TERMCTL`.
* `terminal`: Displays active mode and column width.
* `terminal local`: Direct output to the graphical framebuffer console only.
* `terminal serial`: Direct output to COM1 serial port (115200 baud) only.
* `terminal mirror`: Mirror output synchronously to both framebuffer and COM1 serial.
* `terminal plain`: Raw plain mode without ANSI escape sequences.  
**Examples:**
```sh
terminal
terminal mirror
terminal local
```

---

### `set`
**Syntax:** `set`  
**Child-Safe in Pipelines:** No  
**Description:** Displays all shell variables (local and exported) currently set in the shell session.  
**Examples:**
```sh
set
```

---

### `unset`
**Syntax:** `unset <name> ...`  
**Child-Safe in Pipelines:** No  
**Description:** Removes the specified variables from the shell environment.  
**Examples:**
```sh
unset MY_VAR
```

---

### `export`
**Syntax:** `export [name[=val] ...]`  
**Child-Safe in Pipelines:** No  
**Description:** Sets environment variables and marks them to be exported to child processes.
* `export` with no arguments lists all currently exported environment variables.
* `export VAR=val` assigns `val` to `VAR` and exports it.
* `export VAR` exports an existing variable.  
**Examples:**
```sh
export
export PATH=/bin:/mnt/bin
export DEBUG=1
```

---

### `env`
**Syntax:** `env [-i] [KEY=VALUE...] [COMMAND [ARG...]]`  
**Path:** `/bin/env` (also builtin)  
**Child-Safe in Pipelines:** Yes  
**Description:** Runs a program in a modified environment, or prints all exported environment variables passed to the process.
* `env` without arguments prints the current exported environment variables.
* `env KEY=VALUE...` prints the environment with specified variable overrides or additions.
* `env -i [KEY=VALUE...]` starts with an empty environment instead of inheriting the parent's environment.
* `env [KEY=VALUE...] COMMAND [ARG...]` executes `COMMAND` with the modified environment array and returns the command's exit code.  
**Examples:**
```sh
env
env | grep PATH
env FOO=bar myapp
env -i PATH=/bin myapp
```

---

### `which`
**Syntax:** `which [-a] <command>...`  
**Path:** `/bin/which`  
**Child-Safe in Pipelines:** Yes  
**Description:** Locates an executable file in the colon-separated directories specified by `$PATH` (defaulting to `/bin:/sbin`).
* If a path contains a `/`, tests directly if the file exists and is executable.
* `-a`, `--all`: Print all matching executables found in `$PATH` rather than stopping at the first match.
* Returns exit code `0` if all requested commands are found, `1` if any command was not found.  
**Examples:**
```sh
which ls
which ifconfig
which -a cat
which /bin/nano
```

---

### `alias`
**Syntax:** `alias [name[='value'] ...]`  
**Child-Safe in Pipelines:** No  
**Description:** Defines or displays command aliases.
* `alias` without arguments prints all currently defined aliases.
* `alias name='command'` creates an alias.
* `alias name` displays the definition of the alias `name`.  
* Default aliases configured at shell startup include `tracert='traceroute'`.
**Examples:**
```sh
alias
alias ll='ls /bin'
alias p='sysinfo'
```

---

### `unalias`
**Syntax:** `unalias <name> ...`  
**Child-Safe in Pipelines:** No  
**Description:** Removes one or more command aliases defined in the shell.  
**Examples:**
```sh
unalias ll
```

---

### `jobs`
**Syntax:** `jobs`  
**Child-Safe in Pipelines:** No  
**Description:** Displays all active background and stopped jobs managed by the current shell session, showing job number (`[1]`), current flag (`+` or `-`), state (`Running`, `Stopped`, or `Done`), and command line.  
**Examples:**
```sh
jobs
```

---

### `fg`
**Syntax:** `fg [%n | %+ | %-]`  
**Child-Safe in Pipelines:** No  
**Description:** Brings a background or stopped job into the foreground, transferring terminal foreground ownership and sending `SIGCONT` if stopped. Defaults to the current job (`%+`).  
**Examples:**
```sh
fg
fg %1
```

---

### `bg`
**Syntax:** `bg [%n | %+ | %-]`  
**Child-Safe in Pipelines:** No  
**Description:** Resumes a stopped job in the background by sending `SIGCONT`. Defaults to the current job (`%+`).  
**Examples:**
```sh
bg
bg %1
```

---

### `kill`
**Syntax:** `kill PID|%job [SIGNAL]` or `kill -SIGNAL PID|%job` or `kill -s SIGNAL PID|%job`
**Child-Safe in Pipelines:** No  
**Description:** Sends a signal to a positive PID, or to an owned job's process group using `%n`. The standalone `/bin/kill` accepts positive PIDs and can be invoked by `sudo`; shell job targets require the builtin. PID 0 and negative PID targets are rejected.
* Supported signal names: `HUP`, `INT`, `KILL`, `PIPE`, `TERM`, `CHLD`, `CONT`, `STOP`, `TSTP`, `TTIN`, `TTOU` (or numeric equivalent 1..31).
* If signal is omitted, defaults to `TERM` (`SIGTERM`).  
**Examples:**
```sh
kill %1
kill %1 STOP
kill %1 CONT
kill %1 KILL
ps -o pid,ppid,name
kill 123
sudo kill -9 123
```

Replace `123` with the intended PID. For SSH recovery, identify a child worker
whose PPID is the SSH daemon's PID; terminate that worker to release the session
slot while retaining the listening daemon. `jobs` lists only the current shell's
jobs and must be run directly, not as `sudo jobs`.

#### Background Output and Automatic Prompt Redraw
When a background job (such as `wget &` or background workers writing to `/dev/tty`) produces output while the shell is waiting for user input at the prompt:
* The kernel terminal driver tracks console screen output generations (`console_inc_generation`).
* The shell's interactive input loop periodically samples `SYS_TERMCTL(TERM_GET, &term)`.
* If background output modifies the display (`cur_term.generation != term.generation`), the shell automatically advances to a new line and redraws the prompt along with any in-progress input draft and cursor position within 100 ms.
* This eliminates prompt corruption caused by asynchronous background tasks without requiring workarounds like `wget -q`.

---

### `version`
**Syntax:** `version`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints the FortressOS kernel version, release name, target architecture (`x86_64 SMP`), build compiler, and compilation timestamp.  
**Examples:**
```sh
version
```

---

## 4. Line Editor (`edit`) Subcommands

When running `edit <path>`, the shell enters an interactive line editor with a dedicated `edit> ` prompt. The editor holds up to 64 lines (maximum 127 characters per line, 8192 bytes total).

| Subcommand | Syntax | Description |
|:---|:---|:---|
| `p` / `print` | `p [line]` | Print entire buffer with line numbers, or print a specific line |
| `a` / `append` | `a` | Enter append mode to add lines at the end. Exit mode with `.` on a line by itself or `Ctrl+D` |
| `i` / `insert` | `i <line_num>` | Insert lines *before* `<line_num>`. Exit mode with `.` or `Ctrl+D` |
| `d` / `delete` | `d <line_num>` | Delete the line at `<line_num>` |
| `c` / `change` | `c <line_num>` | Replace the contents of `<line_num>` with new input |
| `w` / `write` / `save` | `w` | Save buffer to disk at the file path specified when entering the editor |
| `stats` | `stats` | Display file statistics: path, line count, byte count, modified status |
| `help` | `help` | Show summary of editor commands |
| `q` / `quit` | `q` | Exit the editor and return to the shell. Warns if unsaved changes exist |

---

## 5. Core Utilities & Stream Tools

Located in `/bin/`, these standalone ELFs process input streams with high performance and strict bounds.

### `md5sum` / `sha256sum`

**Syntax:** `sha256sum [--] [FILE ...]` or `sha256sum -c MANIFEST`; `md5sum` accepts the same arguments.

Both hash exact bytes using fixed buffers, regardless of file size. No operands or `-` reads stdin. Output is lowercase digest, two spaces, filename; stdin is named `-`. Prefer SHA-256 for downloads; MD5 is compatibility-only. A digest verifies bytes against an expected value, not the trustworthiness of its source.

Verification reads paths relative to the current directory, accepts text/binary manifest markers and prints `FILE: OK` or `FILE: FAILED`. Escaped filenames, newline/backslash-containing paths and stdin file entries in manifests are unsupported. Lines are limited to 512 bytes, paths to 255 bytes; malformed/empty manifests fail. Exit 0 means every requested hash/check succeeded, 1 means mismatch or I/O/manifest failure, 2 means invalid command options. Referenced files must be regular files; unrelated operands/entries continue after failure.

```sh
sha256sum /mnt/archive.tar
sha256sum /mnt/archive.tar > /mnt/archive.sha256
sha256sum -c /mnt/archive.sha256
cat /mnt/archive.tar | sha256sum
md5sum /mnt/archive.tar
```

---

### `tar`
**Syntax:** `tar -tf ARCHIVE` or `tar -xf ARCHIVE -C NEW_DIR`  
**Path:** `/bin/tar`  
**Description:** Lists or extracts uncompressed POSIX USTAR archives into a newly created destination directory using bounded streaming buffers and strict two-pass validation.  
**Options:**
* `-t`: List archive contents (`-tf ARCHIVE`).
* `-x`: Extract archive members (`-xf ARCHIVE -C NEW_DIR`).
* `-f ARCHIVE`: Specify path to archive file.
* `-C NEW_DIR`: Destination directory for extraction (must not exist beforehand).
* `--help`: Display usage summary.

**Safety and Constraints:**
* **Format:** Uncompressed POSIX USTAR regular files and directories only.
* **Rejections:** Rejects symlinks, hard links, device files, FIFOs, sparse files, PAX/GNU extensions, and compressed archives.
* **Path Security:** Rejects absolute paths, parent traversal (`..` or `.`), empty components, duplicate member paths, and file/directory prefix conflicts.
* **Caps & Bounds:** Maximum 256 members, depth 16, 16 MiB per file, 64 MiB total expanded data, full path < 256 bytes, each component < 64 bytes, 4096-byte streaming buffer. Requires two 512-byte zero end blocks and only zero padding thereafter.
* **Changed-Archive Policy (Option A):** Two-pass validation before mutation. If the SHA-256 recorded during validation pass 1 differs from pass 2, extraction aborts before creating the destination directory. If extraction fails partway (including I/O or source change during extraction), the tool reports the retained partial destination directory path and exits nonzero without recursive deletion.

**Examples:**
```sh
tar -tf /mnt/archive.tar
tar -xf /mnt/archive.tar -C /mnt/extracted
```

---

### `cat`
**Syntax:** `cat [--] [FILE ...]`  
**Path:** `/bin/cat`  
**Description:** Concatenates files and writes them to standard output with exact byte fidelity. If no files are specified or if `-` is given, reads from standard input. Does not sanitize control or binary bytes.  
**Options:**
* `--`: Ends option scanning.
* `-`: Reads from standard input.  
**Examples:**
```sh
cat /etc/motd
cat /etc/network.conf
echo "Pipeline text" | cat
cat file1.txt file2.txt > combined.txt
```

---

### `head`
**Syntax:** `head [-n N | -c N] [--] [FILE ...]`  
**Path:** `/bin/head`  
**Description:** Prints the first part of files or standard input. Defaults to 10 lines. Specifying count `0` reads nothing.  
**Options:**
* `-n <N>`: Print the first `<N>` lines.
* `-c <N>`: Print the first `<N>` bytes.
* `--help`: Display usage summary.  
**Examples:**
```sh
head /etc/motd
head -n 5 /etc/network.conf
cat /etc/motd | head -c 20
```

---

### `tail`
**Syntax:** `tail [-f] [-n N | -c N] [--] [FILE ...]`  
**Path:** `/bin/tail`  
**Description:** Prints the last part of files or standard input. Defaults to 10 lines. Tail reads to EOF; when `-f` is specified, continues following newly appended data.  
**Limits:**
* Maximum retained lines: 10 lines (up to 4096 bytes per line).
* Maximum retained bytes: 65,536 bytes (`-c 65536`).  
**Options:**
* `-f`: Follow the file continuously as new data is appended at a bounded 100 ms poll interval via `SYS_NANOSLEEP`. Detects file truncation (`st.size < offset`), reports `tail: <file>: file truncated` to standard error, resets the read offset to 0, and follows from the beginning of the file. Detects file deletion/unlinking (`SYS_STAT` failure), reports `tail: <file>: file has been deleted` to standard error, and exits with code 1. When applied to standard input (`tail -f -` or no operands), processes existing stdin, emits diagnostic `tail: cannot follow standard input` on EOF, and exits cleanly with code 0. Cleanly terminates on `SIGINT` (Ctrl+C) with exit status 130 and `SIGTERM` with status 0.
* `-n <N>`: Output the last `<N>` lines (maximum 10).
* `-c <N>`: Output the last `<N>` bytes (maximum 65536).
* `--help`: Display usage summary.  
**Examples:**
```sh
tail /etc/motd
tail -n 2 /etc/network.conf
cat /mnt/log.txt | tail -c 128
tail -f /mnt/test.log
tail -n 5 -f /var/log/messages
```

---

### `wc`
**Syntax:** `wc [-l] [-w] [-c] [--] [FILE ...]`  
**Path:** `/bin/wc`  
**Description:** Counts newlines (lines), whitespace-delimited words, and bytes in files or standard input. If no flags are given, prints all three counts (`-lwc`).  
**Options:**
* `-l`: Count lines (newline `\n` characters).
* `-w`: Count words (ASCII whitespace-delimited sequences).
* `-c`: Count bytes.
* `--help`: Display usage summary.  
**Examples:**
```sh
wc /etc/motd
wc -l /etc/network.conf
echo "one two three" | wc -w
cat /bin/hello | wc -c
```

---

### `grep`
**Syntax:** `grep [OPTIONS] PATTERN [FILE ...]`  
**Path:** `/bin/grep`  
**Description:** Searches files or standard input for lines matching a pattern (substring or regular expression). Operates on arbitrary stream lengths with zero heap allocation using static 4 KiB chunking. Returns exit code 0 if any matching lines are found, 1 if no match, and 2 on error.  
**Options:**
* `-i`: Ignore case distinctions.
* `-v`: Invert the sense of matching (select non-matching lines).
* `-c`: Print only a count of selected lines per file.
* `-n`: Prefix each output line with its 1-based line number.
* `-l`: Print only the names of files containing at least one match.
* `-q`: Quiet mode; suppress all output, exit 0 immediately on first match, exit 1 if no match.
* `-h`: Suppress filename prefixes when searching multiple files.
* `-H`: Always print filename prefix for each match.
* `-F`: Interpret `PATTERN` as a fixed string instead of a regular expression.
* `-E`: Interpret `PATTERN` as an extended regular expression.
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.
* `-`: Read from standard input.  
**Pattern Metacharacters:**
* `^`: Anchor match to beginning of line.
* `$`: Anchor match to end of line.
* `.`: Matches any single character (except newline).
* `*`: Matches zero or more occurrences of the preceding element.
* `[...]` / `[^...]`: Character classes (e.g. `[0-9]`, `[a-zA-Z]`, `[^0-9]`).
* `\`: Escapes metacharacters (`\.`, `\*`, `\\`, etc.).  
**Examples:**
```sh
grep Fortress /etc/motd
cat /etc/motd | grep -i fortress
ps | grep shell
grep -c -v '^#' /etc/network.conf
grep -n '1500' /etc/network.conf
grep -l '10.0.2' /etc/*.conf
echo "hello world" | grep -q "world"
```

---

### `sort`
**Syntax:** `sort [OPTIONS] [FILE...]`  
**Path:** `/bin/sort`  
**Description:** Sorts lines of text files from files or standard input and writes the result to standard output or a designated output file. Employs an iterative, stack-free bottom-up merge sort algorithm ensuring strict $O(N \log N)$ complexity, stability on equal keys, and bounded BSS memory without dynamic heap allocation.
* **Buffer Capacity:** Maximum total input is 256 KiB text pool (`SORT_POOL_SIZE = 262144` bytes) and up to 8,192 lines (`SORT_MAX_LINES = 8192`). Inputs exceeding either limit cleanly fail with a diagnostic.  
**Options:**
* `-r`, `--reverse`: Reverse the result of comparisons while preserving stability.
* `-n`, `--numeric-sort`: Compare according to string numerical value (supports signed 64-bit integers and leading whitespace). Equal numeric values tie-break on the full line content (also respecting `-f`), or deduplicate under `-u`.
* `-u`, `--unique`: Output only the first line of an equal run (or verify strict order when used with `-c`).
* `-f`, `--ignore-case`: Fold ASCII `A-Z` to `a-z` only. Non-ASCII bytes (`0x80..0xFF`) are compared byte-wise.
* `-c`, `--check`: Check if input is already sorted; exits with 0 if sorted, or 1 and prints a diagnostic with the line number (`disorder on line N`) if a disorder is detected.
* `-C`: Check if input is already sorted silently; exits with 1 on disorder without emitting any diagnostic message.
* `-k POS`: Sort by key field starting at 1-indexed field `POS` (whitespace-delimited) and extending to the end of the line (GNU-style).
* `-o FILE`: Write sorted result to `FILE` instead of standard output (safe even when `FILE` is one of the inputs).
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.
* `-`: Read from standard input.  
**Examples:**
```sh
sort /etc/motd
cat /etc/passwd | sort -u
ps | sort -k 1 -n
sort -r -n numbers.txt
sort -c /etc/network.conf
sort -C /etc/network.conf || echo "Disordered"
sort -o sorted.txt input.txt
```

---

### `uniq`
**Syntax:** `uniq [OPTIONS] [INPUT [OUTPUT]]`  
**Path:** `/bin/uniq`  
**Description:** Filters adjacent matching lines from `INPUT` (or standard input) and writes to `OUTPUT` (or standard output). Operates on arbitrary stream lengths with zero heap allocation using a dual-line sliding BSS window.  
**Options:**
* `-c`: Prefix lines by the number of occurrences.
* `-d`: Only print duplicate lines.
* `-u`: Only print unique lines.
* `-i`: Ignore differences in case when comparing.
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.
* `-`: Read from standard input or write to standard output.  
**Examples:**
```sh
uniq /etc/motd
cat /etc/network.conf | uniq -c
ps | uniq -u
uniq -d duplicate_words.txt
uniq input.txt output.txt
```

---

### `xxd`
**Syntax:** `xxd [OPTIONS] [INFILE [OUTFILE]]`  
**Path:** `/bin/xxd`  
**Description:** Creates a hex dump of a file or standard input, or reverses a hex dump back into binary data. Operates on arbitrary stream lengths with zero heap allocation using static BSS buffers and streaming chunk I/O conforming to the freestanding Ring 3 ABI.  
**Options:**
* `-c COLS`: Format `<COLS>` octets per line [1..256] (default: 16 in normal mode, 30 in plain mode).
* `-g BYTES`: Number of octets per group in normal mode (default: 2, 0 to disable grouping).
* `-l LEN`: Stop after writing `<LEN>` octets.
* `-s SEEK`: Start at `<SEEK>` bytes offset (decimal or `0x` hexadecimal).
* `-p`: Output plain continuous hex dump style without offsets or ASCII sidebar.
* `-r`: Reverse operation: convert hex dump back to binary.
* `-u`: Use uppercase hex digits (`0123456789ABCDEF`).
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.
* `-`: Read from standard input or write to standard output.  
**Examples:**
```sh
xxd /etc/motd
xxd -l 32 /etc/motd
xxd -s 0x10 -l 16 /etc/motd
head -c 32 /etc/motd | xxd -p
echo "48656c6c6f" | xxd -r -p
xxd input.bin dump.hex
xxd -r dump.hex restored.bin
```

---

### `diff`
**Syntax:** `diff [OPTIONS] FILE1 FILE2`  
**Path:** `/bin/diff`  
**Description:** Compares two files line by line, or compares a file with standard input (using `-`). Employs an iterative, stack-free Myers divide-and-conquer diff algorithm ensuring optimal edit scripts with bounded BSS memory (128 KiB text pool and 2,048 lines per file) without dynamic heap allocation.
* **Exit Codes:** `0` if files are identical, `1` if differences are found, or `2` upon error (missing file, unreadable operand, or file exceeding buffer capacity).  
**Options:**
* `-u`, `-U NUM`: Output unified diff format with `NUM` (default 3) lines of context, displaying `---`, `+++`, `@@ -a,b +c,d @@`, and `+`/`-` edit markers.
* `-q`, `--brief`: Report only whether files differ (`Files FILE1 and FILE2 differ`) without detailing line-by-line differences.
* `-i`, `--ignore-case`: Ignore case differences in line contents (folds ASCII `A-Z` to `a-z`; non-ASCII bytes are compared byte-wise).
* `-w`, `--ignore-all-space`: Ignore all white space (`' '`, `'\t'`, `'\r'`) when comparing lines.
* `-b`, `--ignore-space-change`: Ignore changes in the amount of white space (treats any whitespace sequence as equivalent).
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.
* `-`: Read from standard input.  
**Examples:**
```sh
diff /etc/motd /etc/motd
diff -u /etc/network.conf /mnt/network.conf
diff -q /etc/motd /etc/network.conf
cat /etc/motd | diff /etc/motd -
diff -i -w file1.txt file2.txt
```

---

### `patch`
**Syntax:** `patch [OPTIONS] [ORIGFILE [PATCHFILE]]`  
**Path:** `/bin/patch`  
**Description:** Applies a diff file to an original file. Supports both unified diff format (`diff -u`, `git diff`) and traditional normal diff format (`diff`). Operates under a bounded static memory architecture (256 KiB text pool, 4,096 lines) with zero dynamic allocation. Employs a fail-closed transactional mutation model with atomic temp file + rename (`.tmp` -> `target` via `SYS_RENAME`): all hunks are validated against the original text before any write begins; output is emitted to a temporary file and atomically renamed into place upon successful completion. If any hunk fails to match or an I/O error occurs, the temporary file is unlinked, the original file is left completely untouched, and `patch` exits with status `1`.
* **In-Place Mutation & Fallback:** In-place patching uses a temporary file (`<target>.tmp`) written in the same directory as the target, then renamed into place. If the temp file cannot be created, or if the rename fails on the target filesystem, patch falls back to non-atomic direct write. This is rare on ext2 but possible on resource-constrained filesystems. A warning is emitted in those cases. If an atomic rename fails after unlinking the target, a fatal diagnostic explicitly discloses the location of the retained patched content in `<target>.tmp` to permit manual recovery.
* **Exit Codes:** `0` if all hunks applied successfully (or dry-run succeeded), `1` if one or more hunks failed, or `2` upon fatal error (missing file, syntax error, or memory limit exceeded).
**Options:**
* `-p NUM`, `--strip=NUM`: Strip `NUM` leading path components from filenames extracted from patch headers (`---` / `+++`).
* `-i FILE`, `--input=FILE`: Read patch from `FILE` instead of standard input.
* `-o FILE`, `--output=FILE`: Write patched output to `FILE` instead of in-place modifying the target file (use `-o -` for standard output).
* `-R`, `--reverse`: Reverse the patch (treat removals as additions and additions as removals).
* `-u`, `--unified`: Interpret the patch as unified diff.
* `-s`, `-q`, `--quiet`, `--silent`: Suppress non-error diagnostics (only emit hunk failure warnings and summary).
* `--dry-run`: Test the patch against target without modifying any files or writing output.
* `--help`: Display usage summary and exit.
* `--`: Ends option scanning.  
**Examples:**
```sh
patch file.txt patch.diff
patch -p1 < feature.patch
patch -R -o restored.txt current.txt patch.diff
patch --dry-run /etc/network.conf network.patch
cat bugfix.diff | patch -o - source.c
```

---

### `sed`
**Syntax:** `sed [OPTIONS] [SCRIPT] [FILE ...]`  
**Path:** `/bin/sed`  
**Description:** Bounded stream editor for filtering and transforming text lines from standard input or files. Reuses the iterative, bounded bitmask NFA regular expression engine from `grep` augmented with $O(1)$-stack submatch capture tracking. Operates under a strictly bounded static memory architecture (64 KiB line buffer, 64 KiB lookahead buffer, zero dynamic heap allocation) conforming to the freestanding Ring 3 ABI.

* **Supported Subset:**
  * **Commands:**
    * `s/pattern/replacement/flags`: Substitution command. Any non-newline/non-backslash character can be used as delimiter (e.g. `s/foo/bar/`, `s:foo:bar:`, `s#foo#bar#`).
      * Flags: `g` (global substitution on the line), `p` (print line if substitution was made), `N` (replace only the Nth match on the line, e.g. `1`, `2`), `i` / `I` (case-insensitive matching).
      * Replacement backreferences: `&` (inserts whole matched text), `\1`–`\9` (inserts matched capture group), `\\` (literal backslash), `\&` (literal `&`), `\n` (newline), `\t` (tab).
    * `p`: Print the current line to standard output (or temporary file if `-i`).
    * `d`: Delete the current line (suppresses further command processing on this line and prevents default printing).
  * **Addressing:**
    * `N`: 1-based line number (e.g. `2p`, `1s/a/b/`).
    * `N,M`: Line number range from $N$ to $M$ inclusive (e.g. `1,2d`, `2,5s/foo/bar/`).
    * `/pattern/`: Selects lines matching the regular expression (e.g. `/error/p`, `/^#/d`).
    * `/pattern1/,/pattern2/`: Range starting on the first line matching `pattern1` and continuing through the next line matching `pattern2`.
    * `$`: Selects the last line of input (evaluated via 1-line lookahead streaming buffer).
    * `!`: Inverts address selection (e.g. `2!d` deletes all lines except line 2; `/pattern/!p` prints lines not matching pattern).
  * **Options:**
    * `-n`, `--quiet`, `--silent`: Suppress default printing of lines. Only lines explicitly printed by `p` or `s/.../.../p` are emitted.
    * `-e <script>`: Specify a command script. Multiple `-e` options are concatenated and executed in sequential order on each line.
    * `-i[SUFFIX]`: Edit files in place. Uses a fail-closed transactional temporary file (`<file>.tmp`) renamed atomically into place (`SYS_RENAME`). If `SUFFIX` is provided (e.g. `-i.bak`), saves a backup copy of the original file at `<file><SUFFIX>` before replacing it. If `-i` is given without suffix, modifies in place without creating a backup.
    * `--help`: Display usage summary and exit.
    * `--`: Ends option scanning.
    * `-`: Read from standard input.

* **Unsupported Commands (Rejected with Error):**
  FortressOS `/bin/sed` enforces strict conformance to the supported stream subset. Every unsupported or unrecognised command is explicitly rejected with a clear diagnostic (e.g. `sed: unsupported command 'a'`) and exits nonzero (code 2):
  * `a`, `i`, `c` (append, insert, change text input commands)
  * `r`, `w` (read file, write file)
  * `y` (transliterate characters)
  * `h`, `H`, `g`, `G`, `x` (hold space and pattern exchange)
  * `b`, `t`, `:` (branching, conditional jumps, labels)
  * `{`, `}` (command blocks)
  * `=` (print current line number)
  * Step addressing (`first~step`)
  * GNU extensions

**Examples:**
```sh
echo "hello world" | sed 's/world/there/'           # Outputs: hello there
echo "aaa" | sed 's/a/b/g'                          # Outputs: bbb
printf 'line1\nline2\nline3\n' | sed -n '2p'        # Outputs: line2
printf 'foo\nbar\nbaz\n' | sed 's/^/prefix-/'       # Outputs: prefix-foo\nprefix-bar\nprefix-baz\n
printf 'a\nb\nc\n' | sed '1,2d'                     # Outputs: c
echo "hello" | sed 's/\(hel\)lo/\1p/'               # Outputs: help
sed -i 's/10.0.2.15/192.168.1.100/' /etc/network.conf
sed -i.bak 's/foo/bar/g' /mnt/config.txt
cat /etc/motd | sed 's/Fortress/FORTRESS/i' | sort | uniq
```

---

### `awk`
**Syntax:** `awk [OPTIONS] [PROGRAM] [FILE ...]`  
**Path:** `/bin/awk`  
**Description:** Bounded pattern scanning and processing tool for filtering, computing, and transforming delimited text streams. Reuses the iterative bitmask NFA regular expression engine from `grep` and the formatting semantics of `printf`. Operates under a strictly bounded static memory architecture (64 KiB line buffer, 64-field table, 256 AST nodes, 32 rules, zero dynamic heap allocation, all function stack frames $\le 608$ bytes) conforming to the freestanding Ring 3 ABI.

* **Supported Subset (v1):**
  * **Program Structure:**
    * `pattern { action }`: If the current record matches `pattern`, execute `action`.
    * `{ action }`: Pattern omitted; executes `action` on every input record.
    * `pattern`: Action omitted; defaults to `{ print }` for matching records.
    * Multiple `pattern { action }` pairs evaluated in sequential order per record.
    * Multiple statements within an action block separated by semicolons (`;`).
  * **Patterns:**
    * `/regex/`: Matches the full line ($0) against regular expression (supports `^`, `$`, `.`, `*`, `[abc]`, `[^abc]`, `\(\)`).
    * Expression patterns: Comparison and arithmetic expressions returning truthy/falsy values (e.g. `NR == 5`, `$1 == "foo"`, `$3 > 100`, `NF > 2`, `NR > 1 && $1 == "x"`).
    * String and numeric literals: `"foo"`, `42`, `3.14`.
  * **Fields:**
    * `$0`: The whole current record/line.
    * `$1`, `$2`, ..., `$N`: 1-based fields separated by `FS`.
    * `$NF`: Dynamic field reference pointing to the last field of the current record.
    * `$expr`: Dynamic field evaluation where `expr` evaluates to an integer index.
    * Bounded field table: Supports up to 64 fields per record. Inputs exceeding 64 fields fail closed with `awk: line exceeds maximum fields (64)`.
  * **Field Separators & Built-in Variables:**
    * `-F <sep>`: Set single-character field delimiter (e.g. `-F:`, `-F,`, `-F'\t'`).
    * Default `FS`: Whitespace (runs of contiguous spaces and tabs are collapsed).
    * `NR`: Record (line) number, 1-based monotonic counter across input streams.
    * `NF`: Number of fields in the current record.
    * `FS`: Field separator string (read-only; configured via `-F`).
    * `OFS`: Output field separator string (default: single space `" "`).
  * **Actions & Statements:**
    * `print`: Print `$0` followed by newline.
    * `print expr1, expr2`: Print expressions separated by `OFS` and terminated by newline.
    * `print expr1 expr2`: String concatenation by adjacency (no operator needed).
    * `printf format, expr1, ...`: Formatted output supporting `%s`, `%d`, `%i`, `%u`, `%x`, `%X`, `%c`, `%f`, `%g`, width, precision, and alignment flags.
    * Field assignment: `$1 = "new"`, `$2 = $2 + 10` (automatically reconstructs `$0` when referenced or printed).
  * **Expressions & Operators:**
    * Arithmetic: `+`, `-`, `*`, `/` (integer division), `%` (modulo), unary `-`.
    * Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`.
    * Logical: `&&`, `||`, `!`.
    * Dual-Type Semantics: Values are dual-typed (string and 64-bit integer). In comparisons, if both operands parse as numeric strings, numeric comparison is performed; otherwise, lexicographical string comparison is performed (`compare as string if either is non-numeric, else compare as number`). Truthiness: non-zero integer or non-empty string is true; zero and empty string are false.
  * **Options:**
    * `-F <sep>`: Specify input field separator.
    * `-f <script>`: Read awk program from file instead of command-line argument.
    * `--help`: Display usage summary and exit.
    * `--`: Ends option scanning.
    * `-`: Read from standard input.

* **Unsupported Constructs (Explicitly Rejected with Code 2):**
  FortressOS `/bin/awk` strictly enforces the supported stream subset and rejects unsupported constructs with a diagnostic message and exit code 2:
  * Arrays (`a[$1]`, `for (k in a)`) &rarr; `awk: arrays are unsupported`
  * Control flow (`if`, `while`, `for`, `do`) &rarr; `awk: unsupported control flow '<name>'`
  * Built-in functions (`length`, `substr`, `split`, `gsub`, `sub`, `index`, `toupper`, `tolower`, `sprintf`, `system`, `getline`) &rarr; `awk: unsupported function '<name>'`
  * Special blocks (`BEGIN`, `END`) &rarr; `awk: unsupported block '<name>'`
  * Ternary operator (`? :`) &rarr; `awk: ternary operator is unsupported`
  * `-v` variable assignments &rarr; `awk: option '-v' is unsupported`
  * Multiple `-f` scripts &rarr; `awk: multiple -f scripts are unsupported`

**Examples:**
```sh
printf 'Alice 30\nBob 25\n' | awk '{ print $1 }'           # Outputs: Alice\nBob\n
printf 'a:b:c\n' | awk -F: '{ print $2 }'                  # Outputs: b
printf 'a b c\n' | awk '{ print NF }'                      # Outputs: 3
printf '1\n2\n3\n' | awk 'NR == 2'                         # Outputs: 2
printf 'Alice 30\nBob 25\n' | awk '$2 > 28 { print $1 }'   # Outputs: Alice
echo "hello" | awk '{ printf "%s!\n", $1 }'                # Outputs: hello!
ps | awk '{ print $1, $4 }' | sort
cat /etc/passwd | awk -F: '$3 >= 1000 { print $1, $6 }'
printf 'foo-x\nbar-x\n' | sed 's/-x/ 42/' | awk '{ print $1, $2 }'
```

---

### `diskbench`
**Syntax:** `diskbench [OPTIONS] [MOUNT_POINT]`  
**Path:** `/bin/diskbench`  
**Description:** Filesystem throughput and metadata latency benchmark utility for FortressOS running in Ring 3. Measures sequential write throughput, sequential read throughput, and metadata creation/unlink performance across storage targets (such as NVMe or USB mass storage) with zero dynamic allocation, static BSS storage (16 KiB I/O buffer), and a strict 512B stack budget. Automatic cleanup is guaranteed on normal completion, error, or interruption via signal (`SIGINT`, `SIGTERM`).
* **Exit Codes:** `0` on successful execution, `1` on I/O or filesystem error (e.g. read-only filesystem or inaccessible directory), or `2` on invalid argument or bounds violation.
* **Storage Limits & Memory Bounds:**
  - Sequential write size: default 16 MiB, maximum 64 MiB (parsed with optional `K`/`M`/`G` suffixes).
  - Metadata file count: default 1,000 files, maximum 4,096 files.
  - Chunk size: 16 KiB chunks aligned with kernel `MAX_SYSCALL_WRITE_LEN`.
  - Ext2 awareness: On 1 KiB block ext2 filesystems, files larger than 268 KiB hit the single-indirect mapping cap; `-w` should be set accordingly (e.g. `-w 128K` or `-w 256K`) when testing default ext2 fixtures.
**Options:**
* `-w SIZE`: Write test file size in bytes (e.g. `128K`, `16M`; default: `16M`, maximum: `64M`).
* `-n COUNT`: Metadata test file count (default: `1000`, maximum: `4096`).
* `-t TEST`: Benchmark test filter: `write` | `read` | `meta` | `all` (default: `all`).
* `-d DIR`: Directory path for temporary test files (default: `<MOUNT_POINT>/diskbench-tmp`).
* `-c`: Emit comparison-friendly single-line machine-parseable metrics for automation.
* `-s`: Silent / summary-only mode (suppresses banner and prints concise per-test results).
* `--help`: Display usage summary and exit.
**Examples:**
```sh
diskbench /mnt
diskbench -w 128K -n 50 /mnt
diskbench -t write -w 1M /mnt
diskbench -c -w 256K -n 100 /mnt
diskbench -s -w 64K -n 20 /mnt
```

---

### `disk`
**Syntax:** `disk [subcommand] [OPTIONS]`  
**Path:** `/sbin/disk`, `/bin/disk`  
**Description:** Storage observability utility for FortressOS running in Ring 3. Queries passive kernel device and filesystem registry information without mutating storage or issuing disk writes.
* **Subcommands:**
  * `list` (default): Enumerate registered block devices, capacity, sector size, GPT partition labels, PARTUUIDs, and active mount correlations.
  * `usage` (or `df`): Display mounted filesystem disk space usage, capacity, and inode metrics. Queries active filesystem metrics dynamically via `SYS_MOUNTINFO` (syscall 52) across Ext4, Ext2, and TarFS mounts. Columns include `MOUNT`, `SOURCE`, `FS`, `SIZE` (total capacity), `USED`, `AVAIL` (free space), `USE%`, `INODES` (total inodes), and `IUSE%` (inode utilization).
  * `info <device|uuid>`: Display detailed metadata for a specific block device or partition (type, capacity in human-readable and raw bytes, sector size, partition start LBA, sector count, PARTUUID, GPT label, GPT type GUID with type description, and mount point status). Accepts bare name (e.g. `nvme0n1p1`), `/dev/` path (e.g. `/dev/nvme0n1p1`), or PARTUUID (e.g. `89B65A98-538C-4194-8AD2-142217C5153A` or `PARTUUID=...`).
  * `bench`: Benchmark filesystem throughput (merging diskbench).
    - *Note on naming*: Output prefixes retain 'diskbench' ('=== diskbench: ... ===' and comparison prefix 'diskbench ...') for backward compatibility with automated parsing pipelines.
    - *High-Resolution Timing*: Elapsed time is measured using hardware CPU cycle counters (`rdtsc`) calibrated against kernel invariant TSC (`tsc_hz` from `SYS_SYSINFO`), giving sub-microsecond precision and stable throughput calculations even for small workloads. If TSC is unavailable, it seamlessly falls back to system timer ticks (`tick_hz=100`; 10 ms granularity). Both `time_ms=` and high-resolution `time_us=` are emitted in `-c` comparison output.
* **Options:**
  * `-c`, `--comparison`: Comparison-friendly single-line machine-parseable output (`key=value`). Also serves as shorthand to inspect a device or PARTUUID directly (e.g. `disk -c <UUID>`).
  * `-h`, `--help`: Display usage summary and exit.
**Examples:**
```sh
disk                                                  # List all block devices (flat table including LABEL and PARTUUID)
disk list -c                                          # List devices in comparison key=value format (includes label=, partuuid=)
disk usage                                            # Show mounted filesystem disk space and inode usage table
disk df                                               # Alias for disk usage
disk usage -c                                         # Show filesystem usage in comparison format
disk info nvme0n1p1                                   # Detailed view of partition nvme0n1p1
disk info 89B65A98-538C-4194-8AD2-142217C5153A        # Detailed view of partition looked up by PARTUUID
disk -c 89B65A98-538C-4194-8AD2-142217C5153A          # Shorthand inspection by PARTUUID in comparison format
disk info -c /dev/sda                                 # Detailed view in machine-parseable comparison format
```

---

### `df`
**Syntax:** `df [OPTIONS]`  
**Path:** `/bin/df` (and default shell alias `df='disk usage'`)  
**Description:** Standard POSIX/UNIX disk space inspection tool for FortressOS running in Ring 3. Reports total capacity, used space, available space, utilization percentage, total inodes, and inode utilization for all actively mounted filesystems (`/`, `/mnt`, etc.) via `SYS_MOUNTINFO` (syscall 52). Supports dynamic querying of Ext4, Ext2, and TarFS filesystems.
* **Options:**
  * `-c`, `--comparison`: Emit comparison-friendly single-line machine-parseable output (`key=value`).
  * `-h`, `--help`: Display usage summary and exit.
**Examples:**
```sh
df                                                    # Display mounted filesystem space usage table
df -c                                                 # Display space usage in key=value comparison format
df | grep /mnt                                        # Filter usage for specific mount point
```

---

### `nano`
**Syntax:** `nano [path]`  
**Path:** `/bin/nano`  
**Description:** Standalone full-screen, non-modal interactive visual text editor for FortressOS running in Ring 3. Features real-time cursor navigation, viewport scrolling, in-memory editing, search, cut/uncut clipboard, and file I/O. Uses a pure static BSS buffer model (up to 256 KiB file capacity, 4,096 lines) without dynamic heap allocation.
* **Navigation:** Arrow keys, Home (`Ctrl+A`), End (`Ctrl+E`), Page Up, Page Down.
* **Editing:** Direct ASCII character typing, Backspace (character deletion and row merge), Delete, Enter (row split).
* **WriteOut (`Ctrl+O`):** Prompt for filename and save buffer to persistent storage with `SYS_SYNC`.
* **Exit (`Ctrl+X`):** Clean terminal exit. If buffer is modified, prompts to save changes (`Y`/`N`/`C`).
* **WhereIs / Search (`Ctrl+W`):** Case-insensitive forward and wrap-around search with viewport centering.
* **Cut (`Ctrl+K`) / Uncut (`Ctrl+U`):** Cut line into clipboard and paste line.
* **Redraw (`Ctrl+L`):** Clear and repaint full terminal frame.  
**Examples:**
```sh
nano /mnt/network.conf
nano /mnt/notes.txt
nano
```

---

### `date`
**Syntax:** `date [-u|--utc] [-I|--iso-8601] [-R|--rfc-2822] [+FORMAT] [--help]`  
**Path:** `/bin/date`  
**Description:** Queries kernel wall-clock time via `SYS_CLOCK_GETTIME` (`CLOCK_REALTIME`, syscall 74) and outputs the current date and time in UTC. Supports default POSIX format (`Fri Oct 16 06:56:49 UTC 2026`), ISO-8601 format (`2026-10-16T06:56:49Z`), RFC-2822 format (`Fri, 16 Oct 2026 06:56:49 +0000`), and custom format tokens (including `+%s` for Unix epoch seconds, `+%Y-%m-%d`, `+%H:%M:%S`). Sub-second interpolation is driven by hardware TSC calibration against the APIC timer and PC/AT CMOS RTC.
* **Options:**
  - `-u`, `--utc`: Explicitly output time in Coordinated Universal Time (default).
  - `-I`, `--iso-8601`: Output date and time in ISO-8601 format (`YYYY-MM-DDTHH:MM:SSZ`).
  - `-R`, `--rfc-2822`: Output date and time in RFC-2822 format (`Day, DD Mon YYYY HH:MM:SS +0000`).
  - `+FORMAT`: Custom format string supporting `%Y`, `%m`, `%d`, `%H`, `%M`, `%S`, `%s`, `%a`, `%b`, `%%`, `%t`, `%n`.
  - `--help`, `-h`: Display command usage summary and exit.
**Examples:**
```sh
date
date -I
date -R
date +%s
date +"Today is %Y-%m-%d, time: %H:%M:%S"
```

---

### `sleep`
**Syntax:** `sleep NUMBER[SUFFIX]... [--help]`  
**Path:** `/bin/sleep`  
**Description:** Suspends execution for the specified duration using kernel high-resolution sleep via `SYS_NANOSLEEP` (syscall 75). Supports decimal fractional seconds (e.g. `sleep 0.5`) and standard time unit suffixes (`s` for seconds, `m` for minutes, `h` for hours, `d` for days). If multiple duration arguments are given, they are summed together. Respects signal interruption (e.g. `Ctrl+C` via `SIGINT`), cleanly terminating with exit code 130 and writing the remaining duration if requested.
* **Suffixes:**
  - `s`: Seconds (default when no suffix is specified).
  - `m`: Minutes (multiplies value by 60).
  - `h`: Hours (multiplies value by 3,600).
  - `d`: Days (multiplies value by 86,400).
* **Exit Codes:**
  - `0`: Sleep duration successfully elapsed.
  - `1`: Invalid time interval or missing operand.
  - `130`: Sleep interrupted by a terminating signal (`SIGINT`).
**Examples:**
```sh
sleep 1
sleep 0.25
sleep 10s
sleep 1m 30s
sleep 0.5 0.5
```

---

### `time`
**Syntax:** `time [-p] command [arg...] [--help]`  
**Path:** `/bin/time`  
**Description:** Executes `command` with given arguments and measures elapsed wall-clock time (`real`), user CPU time (`user`), and system CPU time (`sys`). Wall-clock time is measured via `SYS_CLOCK_GETTIME` (`CLOCK_MONOTONIC`, syscall 74) with sub-second resolution calibrated against hardware TSC and APIC timer. Per-process CPU time accounting tracks cycles spent in user mode versus kernel mode across syscalls, interrupts, and context switches, exposed via `SYS_TIMES` (syscall 76) and sampled before child reclamation using `waitpid` (`WNOWAIT`). Writes timing statistics to `stderr` and propagates the executed command's exit status.
* **Options:**
  - `-p`: Use POSIX standard output format (`real %f\nuser %f\nsys %f\n` in seconds with two decimal digits).
  - `-h`, `--help`: Display usage summary and exit.
  - `--`: Stop option parsing; subsequent arguments are treated as command and args.
* **Output Formats:**
  - **Default (Linux-style):** Tab-separated minutes and seconds:
    ```text
    real	0m1.234s
    user	0m0.891s
    sys	0m0.102s
    ```
  - **POSIX (`-p`):** Space-separated seconds with two decimal places:
    ```text
    real 1.23
    user 0.89
    sys 0.10
    ```
* **Exit Codes:**
  - Exits with the child command's exit status (or `128 + signal` if killed).
  - `127`: Command not found.
  - `126`: Command found but cannot be invoked.
  - `2`: Missing command or invalid option.
**Examples:**
```sh
time sleep 1
time -p sleep 1
time sha256sum /bin/shell
time -p -- which date
```

---

### `gzip`
**Syntax:** `gzip [-c] [-d] [-f] [-k] [-1..-9] [--help] [FILE...]`  
**Path:** `/bin/gzip`  
**Description:** Compresses files using standard DEFLATE (RFC 1951) and wraps them in a gzip container (RFC 1952) with an IEEE 802.3 CRC-32 checksum and uncompressed ISIZE trailer. Employs a streaming LZ77 match finder with a fixed 32 KiB sliding window and precomputed Fixed Huffman coding. Memory allocation is entirely static (zero dynamic heap allocations), and stack usage is bounded to <= 400 bytes.
* **Options:**
  - `-c`, `--stdout`: Write compressed stream to standard output; keep original files unchanged.
  - `-d`, `--decompress`, `--uncompress`: Decompress files instead of compressing.
  - `-f`, `--force`: Force overwrite of output files if they already exist.
  - `-k`, `--keep`: Keep (do not delete) original input files upon successful completion.
  - `-1` .. `-9`: Accepted compression presets.
  - `-h`, `--help`: Display usage summary and exit.
  - `--`: Treat all remaining arguments as filenames.
* **Behavior:**
  - With no `FILE`, or when `FILE` is `-`, reads from standard input and writes to standard output.
  - When compressing a named file, creates `FILE.gz` and unlinks `FILE` unless `-k` or `-c` is passed.
* **Examples:**
```sh
gzip -k file.txt          # Compresses file.txt -> file.txt.gz, keeps file.txt
cat file.txt | gzip -c > file.txt.gz
gzip -d file.txt.gz       # Decompresses file.txt.gz -> file.txt
```

---

### `gunzip`
**Syntax:** `gunzip [-c] [-f] [-k] [--help] [FILE...]`  
**Path:** `/bin/gunzip`  
**Description:** Decompresses gzip (RFC 1952) archives formatted with DEFLATE (RFC 1951). Full support for Stored (`BTYPE 00`), Fixed Huffman (`BTYPE 01`), and Dynamic Huffman (`BTYPE 10`) compressed blocks. Enforces strict decompression bomb (zip bomb) mitigation with an unconditional 100 MB output cap (`GZIP_MAX_OUTPUT`). Validates trailer CRC-32 and ISIZE against computed values, cleanly cleaning up partial output and returning non-zero upon corruption or truncation.
* **Options:**
  - `-c`, `--stdout`: Write decompressed stream to standard output; keep original `.gz` files.
  - `-f`, `--force`: Force overwrite of destination files if they already exist.
  - `-k`, `--keep`: Keep original `.gz` input files upon success.
  - `-h`, `--help`: Display usage summary and exit.
* **Examples:**
```sh
gunzip file.txt.gz        # Restores file.txt and unlinks file.txt.gz
gunzip -c archive.tar.gz | tar -tf -
```

---

## 6. System & Process Introspection

### `ps`
**Syntax:** `ps [-o col1,col2,...]`  
**Path:** `/bin/ps`  
**Description:** Takes a snapshot of the kernel process table via `SYS_PROCINFO` and displays process metrics.
* By default, prints `PID`, `PPID`, `PGID`, `SID`, `STATE` (`RUNNING`, `STOPPED`, `ZOMBIE`, `DONE`), and `NAME`.
* `-o`, `--output`, `--format`: Custom comma-separated column selection.
  - Supported columns: `pid`, `ppid`, `pgid`, `sid`, `state` (or `stat`), `name` (or `comm`, `cmd`, `args`).
* `-h`, `--help`: Display usage summary and supported column names.  
**Examples:**
```sh
ps
ps -o pid,name
ps -o comm,state,ppid
ps -o pid,state,args
```

---

### `top`
**Syntax:** `top`  
**Path:** `/bin/top`  
**Description:** Real-time interactive CPU and process monitor. Displays system metrics (uptime, online CPUs, memory utilization) and a live process list sorted by CPU% utilization (with PID tie-breaker).
* **Interactive Mode:** When stdout is a terminal, refreshes automatically every ~1000 ms. Press `q` or `Q` to exit.
* **Batch / One-Shot Mode:** When piped or redirected (e.g. `top | head -n 5`), emits a single snapshot frame and immediately exits cleanly.  
**Examples:**
```sh
top
top | head -n 8
```

---

### `sysinfo`
**Syntax:** `sysinfo [-m|--memory] [-h|--help]`  
**Path:** `/bin/sysinfo`  
**Description:** Queries kernel system statistics and displays structured system information across 5 sections (or detailed memory observability with `-m`):
* **Kernel / OS identity**: FortressOS release banner, SMP configuration, build commit hash and date, formatted uptime (`Hh Mm Ss`).
* **CPU**: Active CPUs online vs detected total, preemption frequency (100 Hz), calibrated TSC frequency in GHz with hardware status (`invariant` or `standard`).
* **Memory**: Total, used, and free managed physical RAM formatted with unit scaling (`GiB`/`MiB`), and kernel heap allocated vs committed memory (`max 512 MiB`).
* **Tasks**: Total enumerable processes with state breakdown (`running`, `sleeping`, `zombie`), and total active thread count.
* **Storage summary**: Block devices, partitions, and active mounted filesystem counts.
* **Memory Observability (`-m`)**: Detailed physical memory (PMM) frames, kernel heap live/free/committed bytes, largest free payload, free block count, VMM page-table frames, and deferred teardown queue length.

**Examples:**
```sh
sysinfo
sysinfo -m
```

Sample output (`sysinfo`):
```text
FortressOS 1.0 (x86_64 SMP, 4 CPUs)
Build: 7f9a446 (2026-10-07)
Uptime: 0h 0m 2s

CPUs online:  4 / 4
Kernel:       SMP, 100 Hz preemption
TSC:          3.45 GHz (standard)

RAM:  total 2 GiB, used 24 MiB, free 2 GiB
Heap: 38 KiB / 60 KiB committed (max 512 MiB)

Processes:  2 (2 running, 0 sleeping, 0 zombies)
Threads:    9

Storage:  1 device, 1 partition, 2 mounted
```

Sample output (`sysinfo -m`):
```text
FortressOS Memory Subsystem Observability

Physical Memory (PMM):
  Total managed:      65536 frames (256.0 MiB)
  Used / allocated:   5126 frames (20.0 MiB)
  Free / available:   60410 frames (235.9 MiB)
  Allocatable (<1G):  60410 frames (235.9 MiB)

Kernel Dynamic Heap:
  Live used:          29920 B (including 32B block metadata)
  Reusable free:      15136 B (within committed capacity)
  Committed backing:  45056 B (11 physical frames)
  Largest free chunk: 15104 B payload (excludes block tags)
  Free blocks count:  1

Virtual Memory Management (VMM):
  Page-table frames:  279 frames (kernel + user hierarchy)
  Deferred teardown:  0 queued address spaces

Snapshot Notice:
  Counters are individually coherent; cross-subsystem values
  are observed sequentially without global lock nesting.
```

---

### `free`
**Syntax:** `free [-h|--human] [-b|--bytes] [-k|--kibi] [-m|--mebi] [-g|--gibi] [--help]`  
**Path:** `/bin/free`  
**Description:** Queries kernel system statistics via `SYS_SYSINFO` (syscall 37) and displays a human-readable memory summary formatted similarly to standard Linux `free`. Reports `total`, `used`, `free`, `shared`, `buff/cache`, and `available` physical managed RAM, as well as `Swap` status. Defaults to human-readable units (`-h` / `--human`).

**Options:**
* `-h`, `--human`: Show all output fields automatically scaled to shortest unit with unit suffix (`B`, `Ki`, `Mi`, `Gi`, `Ti`). (Default)
* `-b`, `--bytes`: Display amount of memory in bytes.
* `-k`, `--kibi`: Display amount of memory in kibibytes (1024 bytes).
* `-m`, `--mebi`: Display amount of memory in mebibytes (1048576 bytes).
* `-g`, `--gibi`: Display amount of memory in gibibytes (1073741824 bytes).
* `--help`: Display usage summary and exit.

**Examples:**
```sh
free
free -m
free -h
free | grep Mem
```

Sample output (`free`):
```text
               total        used        free      shared  buff/cache   available
Mem:           2.0Gi        24Mi       1.9Gi          0B          0B       1.9Gi
Swap:             0B          0B          0B
```

---

## 7. Network Management & Internet Tools

### `ifconfig`
**Syntax:** `ifconfig`  
**Path:** `/sbin/ifconfig`  
**Description:** Queries network interface status via `SYS_NETCTL` (`NETCTL_IFGET`).

Output includes:
* `HWaddr`: MAC address
* `inet`: IPv4 address, netmask, broadcast
* `gateway`: Default gateway
* `dns`: Configured DNS servers (from `resolv.conf`, comma-separated; omitted if none configured)
* `mtu`: Interface MTU
* `link`: Link state (`UP`, `DOWN`, `WAITING`)
* `RX/TX`: Packet counters

The DNS list is read from the same `resolv.conf` the resolver uses via `resolv_conf_path()`.
Path is provisional: `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf`, depending on which exists. After the installer lands, this will move to `/etc/resolv.conf`.  
**Examples:**
```sh
ifconfig
```

---

### `ifup`
**Syntax:**
* `ifup [--dry-run] [config-file]`
* `ifup [--dry-run] <ip>/<prefix> [gateway] [<dns1>] [<dns2>]`
* `ifup [--dry-run] <ip> <netmask> [gateway] [<dns1>] [<dns2>]`  
**Path:** `/sbin/ifup`  
**Description:** Configures the primary network interface statically on the fly via `SYS_NETCTL` (`NETCTL_IFSET`).
* Can load configuration from a file (defaults to `/mnt/.fortress/network.conf` or `/etc/network.conf`).
* Accepts CIDR notation (`10.0.2.15/24 10.0.2.2 [dns1] [dns2]`) or dotted-decimal notation (`10.0.2.15 255.255.255.0 10.0.2.2 [dns1] [dns2]`).
* Validates IP formatting, unicast ranges, netmask bounds, and gateway subnet reachability before applying.
* When optional positional DNS servers are provided (`dns1`, `dns2`), writes them to `/tmp/resolv.conf` (or `/mnt/.fortress/resolv.conf` if `/mnt` is writable) formatted as `nameserver <ip>`.
* Automatically syncs DNS server definitions to `/mnt/.fortress/network.conf`.  
**Options:**
* `-n`, `--dry-run`: Parse and validate the network configuration without applying changes to the kernel.
* `-h`, `--help`: Display usage summary.  
**Examples:**
```sh
ifup --help
ifup --dry-run 10.0.2.50/24 10.0.2.2
ifup 10.0.2.15/24 10.0.2.2
ifup 192.168.0.168/24 192.168.0.1 1.1.1.1 8.8.8.8
ifup 192.168.0.168 255.255.255.0 192.168.0.1 1.1.1.1 8.8.8.8
ifup /etc/network.conf
```

---

### `dhcpc`
**Syntax:** `dhcpc [--dry-run] [--help]`  
**Path:** `/sbin/dhcpc`  
**Description:** Acquires a dynamic IPv4 lease and network configuration from a DHCP server via UDP broadcast and configures `eth0` via `SYS_NETCTL` (`NETCTL_IFSET`).
* Binds to `0.0.0.0:68` and broadcasts `DHCPDISCOVER` to `255.255.255.255:67`.
* Collects `DHCPOFFER`, extracts offered client IP (`yiaddr`), subnet mask (Option 1), router/gateway (Option 3), and DNS nameservers (Option 6).
* Transmits `DHCPREQUEST` and awaits `DHCPACK` confirmation.
* Applies IP, subnet mask, and default gateway to the kernel network stack via `SYS_NETCTL` (`NETCTL_IFSET`).
* Durably saves DNS server addresses to `resolv.conf` (via `resolv_conf_path()`) and network configuration to `/etc/network/network.conf` on persistent/installed systems (and `/mnt/.fortress/network.conf` if present).
* Limitation (v1): One-shot client using bounded busy-wait; no background renewal daemon. The acquired lease lasts until reboot. Requires `sudo` (privileged port 68 and interface configuration). Version 2 (background lease renewal, RFC 2131 T1/T2 timers, and boot-time auto-acquisition) depends on the Time Subsystem and `SYS_NANOSLEEP`.

**Options:**
* `-n`, `--dry-run`: Perform full DHCP handshake (DISCOVER/OFFER/REQUEST/ACK) and display acquired parameters without modifying network interface or writing files.
* `-h`, `--help`: Display usage summary and limitations.

**Examples:**
```sh
# Dry run: discover lease parameters without configuring
sudo dhcpc --dry-run

# Acquire lease and configure network interface
sudo dhcpc
```

---

### `ping`
**Syntax:** `ping <IPv4|hostname> [-c count] [-W timeout]`  
**Path:** `/bin/ping`  
**Description:** Sends ICMP Echo Request packets (32 data bytes) to an IPv4 target or domain hostname and listens for Echo Replies via `SYS_NETCTL` (`NETCTL_PING`).
* If given a hostname, resolves it using the userspace DNS resolver (reading `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf`).
* Displays `PING <hostname> (<ip>) (32 data bytes)` for resolved hostnames and `PING <ip> (32 data bytes)` for numeric IPv4 addresses.
* If resolution fails, reports `ping: cannot resolve <name>`.
* Prints round-trip sequence numbers, RTT in milliseconds, packet loss statistics, and min/avg/max round-trip times.  
**Options:**
* `-c <count>`: Number of echo requests to send (1 to 100, default 4).
* `-W <timeout>`: Timeout in seconds to wait for each reply (1 to 5, default 1).  
**Examples:**
```sh
ping 10.0.2.2
ping google.com
ping 10.0.2.2 -c 10
ping 1.1.1.1 -c 4 -W 2
```

---

### `nslookup`
**Syntax:** `nslookup [-s server-ip] <domain>`  
**Path:** `/bin/nslookup`  
**Description:** Resolves a domain hostname to an IPv4 address (A record) using the FortressOS userspace DNS stub resolver.
* If `-s` is specified, queries the explicit DNS server.
* If `-s` is omitted, reads nameservers from `/tmp/resolv.conf` (or `/mnt/.fortress/resolv.conf`).
* Queries servers in order; if a server times out, automatically falls back to the next configured server. Returns a timeout error if all servers fail.
* Supports UDP queries with automatic fallback to TCP on truncated responses (`TC` bit set).  
**Options:**
* `-s <server-ip>`: Explicitly specifies the DNS server IPv4 address to query.  
**Examples:**
```sh
nslookup example.com
nslookup -s 10.0.2.3 myhost.local
nslookup -s 8.8.8.8 google.com
```

---

### `nc` (Netcat)
**Syntax:**
* Connect: `nc [options] <IPv4|hostname> <port>`
* Connect with DNS override: `nc -s <dns-server-ip> <hostname> <port>`
* Listen: `nc -l <port>`
* Help: `nc -h` or `nc --help`  
**Path:** `/bin/nc`  
**Description:** TCP stream client and listener designed for finite serial (half-duplex) request/response operations.
* **Client Mode:** Connects to an IPv4 host or domain, drains all data from standard input to EOF, transmits it, half-closes the write direction (`SHUT_WR`), and prints the server response to stdout until EOF.
* **Listener Mode (`-l`):** Binds to the specified TCP port, awaits an incoming connection, transmits any piped/redirected input, and receives incoming data to stdout. (Terminal stdin is automatically skipped when running interactively, functioning as a receive-only listener).
* **Options:**
  * `-s <server-ip>`: Override DNS server IPv4 address for domain hostname resolution (defaults to nameservers configured in `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf` with automatic server fallback on timeout).
  * `-l <port>`: Run in listener mode on the specified TCP port.
  * `-h`, `--help`: Display usage text and architecture notes.

#### Serial Request/Response Architecture & Deadlock Warning
* **Half-Duplex Serial Flow:** `nc` is not an interactive full-duplex terminal client. It strictly serializes transmission before reception: all standard input is sent first, followed by half-close (`SHUT_WR`), before reading begins from the network socket.
* **Deadlock Hazard:** `nc` does not multiplex `stdin` and the socket concurrently. If a peer begins sending a large response (filling the TCP window) *before* consuming the client's entire request, mutual buffer exhaustion will deadlock the transfer. Always use small finite requests or ensure the peer protocol consumes requests before replying.
* **No Client Timeout:** `nc` has no application timeout; it blocks until connection closure, RST, or manual interruption (`Ctrl+C`).  
**Examples:**
```sh
# Send HTTP request and display response
echo -e "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n" | nc example.com 80

# Send finite payload using numeric IPv4
echo hello | nc 192.168.0.222 7777

# Connect with DNS server override
echo fortress-dns | nc -s 192.168.0.153 service.test 9000

# Listen on port 8080 (receive-only from terminal)
nc -l 8080

# Listen with piped response data
echo "server-ready" | nc -l 7777
```

---

### `traceroute`

**Syntax:** `traceroute [-m 1..30] [-q 1..3] [-W 1..5] IPV4`
**Path:** `/bin/traceroute`
**Alias:** `tracert`

Numeric IPv4 only. Defaults: 30 hops, three probes per hop, one second per probe. One 120-second deadline covers the entire command, including ARP; remaining command time bounds every probe. Each probe gets a distinct internal label and prints its own line: `HOP  ADDRESS  RTT ms` or `HOP  *`. RTT follows BSP timer resolution. TTL expiry reports the router; destination Echo Reply ends successfully. An unreachable result ends with `!N`, `!H`, `!P`, `!PORT`, `!FRAG` or `!ROUTE` for ICMP codes 0–5. Other error codes are ignored and may result in a timeout.

Ping and trace share one finite probe resource. Contention prints `ping/trace busy; retry manually` and exits 1 immediately, without retrying. Exit 0 means the destination replied, 1 means an unsuccessful trace/control error, 2 means usage error. Routers may decline ICMP replies; `*` alone does not prove loss of connectivity. The shell provides `tracert` as a default alias for `traceroute`.

```sh
traceroute 192.168.0.1
tracert 192.168.0.1
traceroute -m 10 -q 1 -W 2 192.0.2.9
```

### `wget`
**Syntax:** `wget [options] <URL>` or `wget [destination] <URL>`  
**Path:** `/bin/wget`  
**Description:** Downloads files over HTTP/1.0 and HTTP/1.1.
* Resolves domain hostnames via configured DNS (reads `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf` with automatic server fallback on timeout, or overridden via `-s`).
* Follows HTTP 301 and 302 redirects automatically (up to 5 hops).
* Bounded header scanner (up to 8192 bytes) protects against memory exhaustion.
* Verifies `Content-Length` header framing against received payload size.
* Plain HTTP only (HTTPS is explicitly rejected with clear guidance).
* Automatically detects directory destinations and appends the remote filename.
* Measures elapsed wall time and download speed upon completion.  
**Options:**
* `-O <file|dir>`: Write downloaded content to `<file>` or directory `<dir>`. Use `-O -` to stream directly to standard output.
* `-P <dir>`: Save files to specified directory prefix `<dir>`.
* `-q`, `--quiet`: Quiet mode. Suppresses diagnostic and progress messages on stderr.
* `-s <server-ip>`: Override the DNS server IPv4 address used for hostname resolution.
* `-h`, `--help`: Display usage summary.  
**Examples:**
```sh
wget http://example.com/index.html
wget /mnt http://10.0.2.2:8000/archive.tar
wget -O /mnt/data.bin http://10.0.2.2:8000/archive.tar
wget -P /mnt http://10.0.2.2:8000/archive.tar
wget -q -O - http://example.com/ | wc -c
```

### `download`
**Syntax:** `download [destination] <URL>` or `download <URL> [destination]`  
**Path:** `/bin/download`  
**Description:** Simplified file downloader for FortressOS designed for quick, hassle-free file fetching.
* Automatically appends the remote filename if the destination is a directory (such as `/mnt` or `/mnt/`).
* Automatically defaults destination to `/mnt` if mounted and no destination path is specified, avoiding read-only rootfs errors (`-EROFS`).
* Transparently handles IPv4 addresses, DNS hostnames (resolved via `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf` with fallback), HTTP redirects, and speed calculation.
* Supports destination either as the first or second argument.  
**Options:**
* `-q`, `--quiet`: Quiet mode. Suppresses diagnostic and status output.
* `-s <server-ip>`: Override DNS server IPv4 address.
* `-h`, `--help`: Display usage summary.  
**Examples:**
```sh
# Destination directory first
download /mnt http://192.168.0.153:8000/testfile.txt

# URL first
download http://192.168.0.153:8000/testfile.txt /mnt

# Omit destination entirely (auto-defaults to /mnt if mounted)
download http://192.168.0.153:8000/testfile.txt

# Download directly to a specific filename
download /mnt/custom.bin http://192.168.0.153:8000/data.bin
```

---

## 8. Diagnostic, Test & Internal Binaries

These standalone binaries in `/bin/` verify kernel invariants, subsystem contracts, and ABI constraints.

### `hello`
**Path:** `/bin/hello`  
**Description:** Verifies freestanding Ring 3 ELF loading, argument vector passing, and return status codes.
* `hello`: Prints welcome banner and exits with status 0.
* `hello <arg1> <arg2>`: Prints all passed arguments. If the first argument is numeric, exits with that integer code.
* `hello --spin`: Enters an infinite CPU spin loop (`pause`). Used to generate background CPU load for testing the scheduler and `/bin/top`.

---

### `dual_stream`
**Path:** `/bin/dual_stream`  
**Description:** Diagnostic utility that prints `STDOUT_DATA\n` to descriptor 1 (stdout) and `STDERR_DATA\n` to descriptor 2 (stderr). Used to verify shell stream redirections (`2>&1`, `> out 2> err`).

---

### `tcptest`
**Path:** `/bin/tcptest`  
**Description:** TCP client test fixture verifying connection setup, socket descriptor duplication, buffer boundary validation, and caught signal interrupts (`--hold`, `--caught`, `--reset`, `--unread`, `--connect-caught`).

---

### `tcpserve`
**Path:** `/bin/tcpserve`  
**Description:** Single-client TCP echo server fixture verifying `SYS_LISTEN`, `SYS_ACCEPT`, child socket inheritance, and signal interruptions (`--caught`, `--compete`, `--shared`).

---

### `udptest`
**Path:** `/bin/udptest`  
**Description:** UDP datagram echo fixture.
* Client: `udptest <IPv4> <port> <message>` sends a datagram and waits for echo verification.
* Listener: `udptest --listen <port> <count>` binds to `<port>` and echoes back `<count>` received datagrams.

---

### `dnsprobe`
**Path:** `/bin/dnsprobe`  
**Description:** Ring 3 DNS test fixture testing deadline timeouts and caught `SIGINT` interruption during active DNS resolution.

---

### `tcpdeadline`
**Path:** `/bin/tcpdeadline`  
**Description:** Tests opt-in kernel TCP absolute tick deadlines via `SYS_CONNECT_UNTIL` and `SYS_SEND_UNTIL`.

---

### `net-ping-probe`
**Path:** `/bin/net-ping-probe`  
**Description:** Validates kernel ABI bounds checking for `SYS_NETCTL` (`NETCTL_PING`). Passes invalid addresses, out-of-range lengths, and invalid parameters to verify kernel rejection.

---

### `net-udp-probe`
**Path:** `/bin/net-udp-probe`  
**Description:** Validates kernel UDP socket ABI invariants, unaligned buffer addressing, and `EAGAIN` non-blocking semantics.

---

### `dhcp-probe`
**Path:** `/bin/dhcp-probe`  
**Description:** Verifies kernel broadcast IPv4 receive and transmit paths end-to-end. Binds to `0.0.0.0:68`, broadcasts `DHCPDISCOVER` to `255.255.255.255:67`, polls for up to 5 seconds, prints the responder IP and port on receipt, and exits 0 (or 1 on timeout). Requires `sudo` (privileged port 68).

---

### `sh-builtin`
**Path:** `/bin/sh-builtin`  
**Description:** Isolated pipeline stage runner. Spawns child-safe builtins (such as `echo`, `cat`, `ls`, `pwd`) inside pipeline stages without altering parent shell state.

---

### `init`
**Path:** `/bin/init`  
**Description:** Kernel user initialization binary. Executes Ring 3 bootstrap verification suites and worker self-tests before launching the interactive shell.

---

## 9. Keyboard Shortcuts & Line Editing

The FortressOS Ring 3 shell includes a powerful interactive line editor with history search and shortcut navigation.

### Navigation & Cursor Movement
* **Left / Right Arrow**: Move cursor one character left or right.
* **Home** / **Ctrl+A**: Jump to the beginning of the line.
* **End** / **Ctrl+E**: Jump to the end of the line.

### History Navigation & Search
* **Up Arrow**: Recall previous command from history.
* **Down Arrow**: Recall next command from history.
* **Ctrl+R**: Enter reverse incremental history search mode. Type characters to match previous commands; press Enter to accept or Ctrl+C to cancel.

### Editing & Deletion
* **Backspace**: Delete character to the left of the cursor.
* **Delete**: Delete character under the cursor.
* **Ctrl+W**: Delete word to the left of the cursor.
* **Ctrl+U**: Erase from cursor to beginning of the line.
* **Ctrl+K**: Erase from cursor to end of the line (kill).
* **Ctrl+Y**: Yank (paste) previously erased text.
* **Tab**: Auto-complete command names, builtins, and filesystem paths.

### Process & Terminal Control
* **Ctrl+C**: Cancel current line input, or send `SIGINT` to foreground process.
* **Ctrl+Z**: Suspend current foreground process (`SIGTSTP`), sending it to the background.
* **Ctrl+D**: On an empty line, exits the shell (`exit`). Inside utilities, signals End-Of-File (EOF).
* **Ctrl+L**: Clear the screen and redraw current line.

---
*Documented for FortressOS x86_64 SMP.*
## Permissions and sudo

Live media logs in as `operator` with a visible passwordless warning and a
temporary home at `/run/user/1000`. Root login is locked. An explicit build
hash can require a password for both login and sudo.

`sudo command [args]` authenticates the real user and requires membership in
`wheel`. It runs the command as root with a minimal environment (`PATH=/bin:/sbin`,
`HOME=/root`, `USER=root`, `LOGNAME=root`, `SHELL=/bin/shell`) and returns the
command's exit status. For bare command names, `sudo` probes `/sbin/<cmd>` first,
falling back to `/bin/<cmd>`; use an absolute path for other locations. There is no
authentication cache.

With no arguments, `sudo` prints usage. `sudo -s` opens `/bin/shell` as root;
use `exit` to return to your user shell. In the interactive shell, `sudo !!`
expands the previous history command, displays the expanded command, and runs
it through sudo.
For example, `id` followed by `sudo !!` runs `sudo id`. The expanded command
is saved in history. Quoted `sudo '!!'` stays literal; missing history or an
oversized expansion produces an error. This bounded shorthand applies only
to a whole input line consisting of `sudo !!`, with optional whitespace.

`sudo -b command [args]` authenticates in the foreground, then starts the
command in a separate session with standard input from `/dev/null`. It prints
the child PID and returns after spawning; command readiness is reported by the
command itself. Output and diagnostics retain their existing destinations.
For example, use `sudo -b /sbin/sshd` to start SSH without a root shell; it
continues running after leaving that shell. Do not append `&` when sudo needs
to read a password: background terminal reads stop the job. Ordinary shell
background jobs remain subject to shell-exit cleanup.

Examples: `sudo id`, `sudo cat /etc/shadow`, `sudo ifup /etc/network.conf`, `sudo poweroff`.
Plain `cat /etc/shadow` is denied. USB filesystem mounts ignore set-ID bits.

## Randomness diagnostic

`/bin/getrandom-probe [--worker|--workers|--unavailable]` checks the public
`SYS_GETRANDOM` ABI. Default prints two fresh 32-byte samples after checking
the 256-byte limit and invalid buffers. `--workers` spawns eight ordinary
unpinned workers and checks their exits; `--unavailable` expects fail-closed
hardware absence. Samples are public diagnostic output, never key material.
