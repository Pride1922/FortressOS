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

---

## 2. Quick Reference Index

| Command | Type | Category | Summary |
|:---|:---|:---|:---|
| [`help`](#help) | Builtin (child-safe) | Shell | Show help overview or help for a specific command |
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
| [`sync`](#sync) | Builtin | Filesystem | Flush dirty filesystem buffers to persistent storage |
| [`echo`](#echo) | Builtin (child-safe) | Text | Print arguments to standard output (supports `$?`, `$VAR`) |
| [`run`](#run) | Builtin | Process | Compatibility wrapper to spawn an executable binary |
| [`layout`](#layout) | Builtin | System | Query or set keyboard layout (`us` or `azerty`) |
| [`reboot`](#reboot) | Builtin | System | Save state and reboot system |
| [`shutdown`](#shutdown) / [`poweroff`](#poweroff) | Builtin | System | Save state and ACPI power off |
| [`exit`](#exit) | Builtin | Shell | Exit the current shell session |
| [`dmesg`](#dmesg) | Builtin | System | Display or save kernel diagnostic ring buffer log |
| [`history`](#history) | Builtin | Shell | Display, clear, save, or load command history |
| [`prompt`](#prompt) | Builtin | Shell | Configure shell prompt template |
| [`terminal`](#terminal) | Builtin | Shell | Select terminal display mode (`local`, `serial`, `mirror`, `plain`) |
| [`set`](#set) | Builtin | Environment | Display all shell variables |
| [`unset`](#unset) | Builtin | Environment | Unset shell variables |
| [`export`](#export) | Builtin | Environment | Set or export environment variables to child processes |
| [`env`](#env) | Builtin (child-safe) | Environment | Print exported environment variables |
| [`alias`](#alias) | Builtin | Shell | Define or display command aliases |
| [`unalias`](#unalias) | Builtin | Shell | Remove command aliases |
| [`jobs`](#jobs) | Builtin | Job Control | List active background and stopped jobs |
| [`fg`](#fg) | Builtin | Job Control | Bring background or stopped job to foreground |
| [`bg`](#bg) | Builtin | Job Control | Resume stopped job in the background |
| [`kill`](#kill) | Builtin | Job Control | Send signal to job or process group |
| [`version`](#version) | Builtin (child-safe) | System | Print FortressOS version and build information |
| [`cat`](#cat) | Binary (`/bin/cat`) | Stream Tool | Concatenate and print files with exact byte fidelity |
| [`head`](#head) | Binary (`/bin/head`) | Stream Tool | Output first part of files (lines or bytes) |
| [`tail`](#tail) | Binary (`/bin/tail`) | Stream Tool | Output last part of files (bounded buffer) |
| [`wc`](#wc) | Binary (`/bin/wc`) | Stream Tool | Print newline, word, and byte counts |
| [`nano`](#nano) | Binary (`/bin/nano`) | Editor | Full-screen interactive visual text editor |
| [`ps`](#ps) | Binary (`/bin/ps`) | Introspection | Snapshot active process table |
| [`top`](#top) | Binary (`/bin/top`) | Introspection | Real-time interactive CPU & process monitor |
| [`sysinfo`](#sysinfo) | Binary (`/bin/sysinfo`) | Introspection | Display CPU, uptime, RAM, and process metrics |
| [`ifconfig`](#ifconfig) | Binary (`/bin/ifconfig`) | Networking | Query network interface status and packet counters |
| [`ifup`](#ifup) | Binary (`/bin/ifup`) | Networking | Configure network interface statically or via config file |
| [`ping`](#ping) | Binary (`/bin/ping`) | Networking | Send ICMP Echo Request packets to IPv4 host |
| [`traceroute`](#traceroute) | Binary (`/bin/traceroute`) | Networking | Finite numeric ICMP trace with TTL-expiry/error reporting |
| [`nslookup`](#nslookup) | Binary (`/bin/nslookup`) | Networking | Query DNS name server for IPv4 addresses |
| [`nc`](#nc) | Binary (`/bin/nc`) | Networking | Arbitrary TCP connections and listens (Netcat) |
| [`wget`](#wget) | Binary (`/bin/wget`) | Networking | Download files over HTTP/1.0 and HTTP/1.1 |
| [`md5sum` / `sha256sum`](#md5sum--sha256sum) | Binaries (`/bin/md5sum`, `/bin/sha256sum`) | Files | Stream digests and verify manifests |
| [`hello`](#hello) | Binary (`/bin/hello`) | Diagnostic | Test ELF binary with argument echoing and `--spin` |
| [`dual_stream`](#dual_stream) | Binary (`/bin/dual_stream`) | Diagnostic | Diagnostic tool emitting distinct stdout and stderr streams |
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
**Syntax:** `echo [args...]`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints arguments separated by single spaces, followed by a newline. Evaluates variables expanded by the shell (e.g. `$?`, `$VAR`, `${VAR}`).  
**Examples:**
```sh
echo "Hello, FortressOS!"
echo "Exit status:" $?
echo "Current path:" $PATH
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
**Syntax:** `dmesg [path]`  
**Child-Safe in Pipelines:** No  
**Description:** Prints kernel diagnostic ring buffer messages.
* `dmesg` without arguments prints the kernel log to the terminal.
* `dmesg <path>` saves the complete kernel log to the specified file path on disk (e.g. `/mnt/dmesg.txt`).  
**Examples:**
```sh
dmesg
dmesg /mnt/boot.log
```

---

### `history`
**Syntax:** `history [clear | save | load]`  
**Child-Safe in Pipelines:** No  
**Description:** Manages the shell command history.
* `history` without arguments displays the numbered list of previous commands.
* `history clear` wipes the in-memory history buffer.
* `history save` explicitly writes the history buffer to `/mnt/.fortress/history`.
* `history load` reloads previous command history from `/mnt/.fortress/history`.  
**Examples:**
```sh
history
history save
history clear
```

---

### `prompt`
**Syntax:** `prompt [default | cwd | <template>]`  
**Child-Safe in Pipelines:** No  
**Description:** Configures the shell prompt appearance.
* `prompt` displays the current prompt template.
* `prompt default` resets the prompt to `fortress> `.
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
**Syntax:** `env`  
**Child-Safe in Pipelines:** Yes  
**Description:** Prints all exported environment variables passed to the process.  
**Examples:**
```sh
env
env | head -n 5
```

---

### `alias`
**Syntax:** `alias [name[='value'] ...]`  
**Child-Safe in Pipelines:** No  
**Description:** Defines or displays command aliases.
* `alias` without arguments prints all currently defined aliases.
* `alias name='command'` creates an alias.
* `alias name` displays the definition of the alias `name`.  
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
**Syntax:** `kill [%n] [signal]`  
**Child-Safe in Pipelines:** No  
**Description:** Sends a signal to a job specified by job number (`%n`) or process group.
* Supported signal names: `HUP`, `INT`, `KILL`, `PIPE`, `TERM`, `CHLD`, `CONT`, `STOP`, `TSTP`, `TTIN`, `TTOU` (or numeric equivalent 1..31).
* If signal is omitted, defaults to `TERM` (`SIGTERM`).  
**Examples:**
```sh
kill %1
kill %1 STOP
kill %1 CONT
kill %1 KILL
```

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
**Syntax:** `tail [-n N | -c N] [--] [FILE ...]`  
**Path:** `/bin/tail`  
**Description:** Prints the last part of files or standard input. Defaults to 10 lines. Tail always reads to EOF.  
**Limits:**
* Maximum retained lines: 10 lines (up to 4096 bytes per line).
* Maximum retained bytes: 65,536 bytes (`-c 65536`).  
**Options:**
* `-n <N>`: Output the last `<N>` lines (maximum 10).
* `-c <N>`: Output the last `<N>` bytes (maximum 65536).
* `--help`: Display usage summary.  
**Examples:**
```sh
tail /etc/motd
tail -n 2 /etc/network.conf
cat /mnt/log.txt | tail -c 128
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

## 6. System & Process Introspection

### `ps`
**Syntax:** `ps`  
**Path:** `/bin/ps`  
**Description:** Takes a snapshot of the kernel process table via `SYS_PROCINFO` and displays PID, PPID, process group ID (PGID), session ID (SID), state (`RUNNING`, `STOPPED`, `ZOMBIE`, `DONE`), and process name.  
**Examples:**
```sh
ps
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
**Syntax:** `sysinfo`  
**Path:** `/bin/sysinfo`  
**Description:** Queries kernel system statistics via `SYS_SYSINFO` and formats hardware and operating parameters:
* Number of active online CPUs.
* Monotonic system uptime formatted as `HH:MM:SS`.
* Total managed physical RAM in MiB.
* Free physical RAM in MiB.
* Used physical RAM in MiB.
* Total number of active tasks/processes.  
**Examples:**
```sh
sysinfo
```

---

## 7. Network Management & Internet Tools

### `ifconfig`
**Syntax:** `ifconfig`  
**Path:** `/bin/ifconfig`  
**Description:** Queries the network subsystem via `SYS_NETCTL` (`NETCTL_IFGET`) and displays the status of the network interface (`eth0`):
* Hardware MAC address (`HWaddr`).
* Link carrier status: `UP`, `DOWN`, or `WAITING (no cable)`.
* IPv4 address, subnet mask, prefix length, and default gateway.
* Maximum Transmission Unit (MTU).
* Monotonic 64-bit RX and TX packet and byte counters.  
**Examples:**
```sh
ifconfig
```

---

### `ifup`
**Syntax:**
* `ifup [--dry-run] [config-file]`
* `ifup [--dry-run] <ip>/<prefix> [gateway]`
* `ifup [--dry-run] <ip> <netmask> [gateway]`  
**Path:** `/bin/ifup`  
**Description:** Configures the primary network interface statically on the fly via `SYS_NETCTL` (`NETCTL_IFSET`).
* Can load configuration from a file (defaults to `/mnt/.fortress/network.conf` or `/etc/network.conf`).
* Accepts CIDR notation (`10.0.2.15/24 10.0.2.2`) or dotted-decimal notation (`10.0.2.15 255.255.255.0 10.0.2.2`).
* Validates IP formatting, unicast ranges, netmask bounds, and gateway subnet reachability before applying.
* Automatically syncs DNS server definitions to `/mnt/.fortress/network.conf`.  
**Options:**
* `-n`, `--dry-run`: Parse and validate the network configuration without applying changes to the kernel.
* `-h`, `--help`: Display usage summary.  
**Examples:**
```sh
ifup --help
ifup --dry-run 10.0.2.50/24 10.0.2.2
ifup 10.0.2.15/24 10.0.2.2
ifup /etc/network.conf
```

---

### `ping`
**Syntax:** `ping <ip> [-c count] [-W timeout]`  
**Path:** `/bin/ping`  
**Description:** Sends ICMP Echo Request packets (32 data bytes) to an IPv4 target and listens for Echo Replies via `SYS_NETCTL` (`NETCTL_PING`). Prints round-trip sequence numbers, RTT in milliseconds, packet loss statistics, and min/avg/max round-trip times.  
**Options:**
* `-c <count>`: Number of echo requests to send (1 to 100, default 4).
* `-W <timeout>`: Timeout in seconds to wait for each reply (1 to 5, default 1).  
**Examples:**
```sh
ping 10.0.2.2
ping 10.0.2.2 -c 10
ping 1.1.1.1 -c 4 -W 2
```

---

### `nslookup`
**Syntax:** `nslookup [-s server-ip] <domain>`  
**Path:** `/bin/nslookup`  
**Description:** Resolves a domain hostname to an IPv4 address (A record) using the FortressOS userspace DNS stub resolver.
* If `-s` is omitted, automatically reads the DNS server configured in `/mnt/.fortress/network.conf` or `/etc/network.conf` (e.g. `10.0.2.3`).
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
* Connect: `nc <IPv4> <port>`
* Connect by Hostname: `nc <hostname> <port>`
* Connect with DNS override: `nc -s <dns-server-ip> <hostname> <port>`
* Listen: `nc -l <port>`  
**Path:** `/bin/nc`  
**Description:** Versatile TCP stream client and listener designed for serial request/response operations.
* **Client Mode:** Connects to an IPv4 host or domain, drains standard input, transmits data, half-closes the write direction, and prints the server response to stdout.
* **Listener Mode (`-l`):** Binds to the specified TCP port, awaits an incoming connection, transmits any piped/redirected input, and receives incoming data to stdout. (Terminal stdin is automatically skipped when running interactively).  
**Examples:**
```sh
echo "GET / HTTP/1.0\r\n\r\n" | nc 93.184.216.34 80
echo "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n" | nc example.com 80
nc -l 8080
```

---

### `traceroute`

**Syntax:** `traceroute [-m 1..30] [-q 1..3] [-W 1..5] IPV4`
**Path:** `/bin/traceroute`

Numeric IPv4 only. Defaults: 30 hops, three probes per hop, one second per probe. One 120-second deadline covers the entire command, including ARP; remaining command time bounds every probe. Each probe gets a distinct internal label and prints its own line: `HOP  ADDRESS  RTT ms` or `HOP  *`. RTT follows BSP timer resolution. TTL expiry reports the router; destination Echo Reply ends successfully. An unreachable result ends with `!N`, `!H`, `!P`, `!PORT`, `!FRAG` or `!ROUTE` for ICMP codes 0–5. Other error codes are ignored and may result in a timeout.

Ping and trace share one finite probe resource. Contention prints `ping/trace busy; retry manually` and exits 1 immediately, without retrying. Exit 0 means the destination replied, 1 means an unsuccessful trace/control error, 2 means usage error. Routers may decline ICMP replies; `*` alone does not prove loss of connectivity. No `tracert` alias is provided in this milestone.

```sh
traceroute 192.168.0.1
traceroute -m 10 -q 1 -W 2 192.0.2.9
```

### `wget`
**Syntax:** `wget [options] <URL>`  
**Path:** `/bin/wget`  
**Description:** Downloads files over HTTP/1.0 and HTTP/1.1.
* Resolves domain hostnames via configured DNS.
* Follows HTTP 301 and 302 redirects automatically (up to 3 hops).
* Bounded header scanner (up to 8192 bytes) protects against memory exhaustion.
* Verifies `Content-Length` header framing against received payload size.
* Plain HTTP only (HTTPS is explicitly rejected with clear guidance).  
**Options:**
* `-O <file>`: Write downloaded content to `<file>`. Use `-O -` to stream directly to standard output.
* `-q`, `--quiet`: Quiet mode. Suppresses diagnostic and progress messages on stderr.
* `-s <server-ip>`: Override the DNS server IPv4 address used for hostname resolution.
* `-h`, `--help`: Display usage summary.  
**Examples:**
```sh
wget http://example.com/index.html
wget -O /mnt/data.bin http://10.0.2.2:8000/archive.tar
wget -q -O - http://example.com/ | wc -c
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
