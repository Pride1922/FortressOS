"""Exhaustive dispatcher inventory with reviewed indirect authority paths.

This detects inventory drift, not semantic correctness; runtime gates are separate.
"""
from pathlib import Path
import hashlib,json,re
root=Path(__file__).resolve().parent.parent
groups={
'actor-filesystem': ('OPEN STAT STAT_EXT MKDIR UNLINK RENAME CHDIR CHMOD FCHMOD CHOWN',
 'syscall_actor -> owned VFS walk -> immutable metadata or authoritative EXT2/EXT4/runfs callbacks; G released before filesystem exclusion'),
'admitted-descriptor': ('READ WRITE READDIR CLOSE DUP DUP2 FCNTL',
 'own fd_table; READ/WRITE rights admitted at open; write_actor gets current actor for set-ID stripping; streams/sockets use shared owned handle; no new pathname authority'),
'spawn': ('SPAWN SPAWN_EXT',
 'process_spawn_from_vfs_group -> G credential snapshot -> caller cwd/open actions -> admitted executable snapshot -> private final child creds -> bind/stage/publish; descriptor cleanup and loader failures unwind'),
'self-credentials': ('UMASK GETRESUID GETRESGID GETGROUPS SETRESUID SETRESGID SETGROUPS CAPSET CAPGET',
 'permissions_syscalls.inc validates all buffers; pure canonical proposal; G compare-and-publish; getters snapshot; groups/IDs/caps bounded; capset drop-only'),
'signal': ('KILL',
 'process_signal_send_creds -> signal_send_common(user=true): current actor/target bound creds under G; same-session gate, real/effective vs real/saved or CAP_KILL; pending atomically published; wake by PID after unlock'),
'self-signal': ('SIGACTION SIGPROCMASK SIGRETURN',
 'own signal state by current tid; G exclusion; validated own user signal-frame context, canonical address/flags; cannot write credentials or foreign contexts'),
'parent-session': ('WAIT WAITPID SETPGID GETPGRP GROUP_RELEASE',
 'process_record_wait restricts children; setpgid self/staged child + same session; group release validates whole staged group parent ownership before any mutation; no root substitute'),
'terminal': ('TERMCTL INPUT_READ TCSETPGRP TCGETPGRP TERMATTR',
 'input.c: controlling session, terminal descriptor where required, foreground TTIN/TTOU and disposition rules; TERM_SET affects self modes; kernel terminal signals explicit trusted entry'),
'public-query': ('PROCINFO SYSINFO MEMINFO MOUNTINFO BLOCKINFO GETCWD',
 'validated output; coherent bounded value snapshots; process listing intentionally all users; blockinfo geometry only, not raw access; fixed /mnt metrics trusted lookup; getcwd self string'),
'public-local': ('KBD_LAYOUT SYNC PIPE EXIT',
 'single-console keyboard layout intentionally public; sync only flushes already admitted mount (no mount or raw-write API); pipe creates own handles; exit only self; protected storage gates remain independent'),
'diagnostics': ('LOCKSTAT SPAWN_PROFILE',
 'lockstat aggregate lock counters intentionally public (no process buffers/passwords/raw memory); profile writes only self TCB or self pipe profile; no authority grants'),
'power': ('REBOOT', 'CAP_SYS_BOOT before freeze/sync/power; invalid command no effect'),
'root-log': ('DMESG', 'effective UID zero before length/buffer handling and before net profile append; no capability surrogate'),
'network': ('SOCKET BIND SENDTO RECVFROM CONNECT LISTEN ACCEPT SEND RECV SHUTDOWN SEND_UNTIL RECV_UNTIL CONNECT_UNTIL',
 'net_socket_syscall BSP + affinity fence; own admitted fd; bind applies CAP_NET_BIND to both UDP/TCP low ports before socket_bind; ephemeral/high ports public; TCP manager/worker explicit internal calls; worker sole protocol owner; leases/lifetime/copies unchanged'),
'network-control': ('NETCTL',
 'BSP+affinity fence; ping/trace bounded public diagnostic mailboxes; IFGET public; IFSET CAP_SYS_ADMIN before validation/apply; net_set_config internal calls from initialization and admitted IFSET only'),
'test-only': ('TEST_SETCREDS', 'compiled only explicit TEST_PERMISSIONS_ENFORCEMENT; fixed self drop, never elevation; production absence verified'),
}
source=root/'src/kernel/syscall.c';text=source.read_text()
cases=set(re.findall(r'case (SYS_\w+):',text))
records={}
for category,(names,authority) in groups.items():
    for name in names.split():
        name='SYS_'+name
        assert name not in records,name
        records[name]={'category':category,'authority':authority,
          'line':next((i for i,line in enumerate(text.splitlines(),1) if 'case '+name+':' in line),None)}
assert cases==set(records),(cases-set(records),set(records)-cases)
abi=set(re.findall(r'^#define (SYS_\w+)\s+\d+', (root/'src/include/syscall_abi.h').read_text(),re.M))
assert abi==cases,(abi-cases,cases-abi)
paths=['src/kernel/syscall.c','src/kernel/permissions_syscalls.inc','src/kernel/thread.c',
       'src/kernel/process_table.c','src/drivers/input.c','src/net/net_socket_syscall.c',
       'src/net/net_tcp_syscall.c','src/net/net.c','src/fs/vfs.c','src/fs/runfs_permissions.inc',
       'src/fs/ext2_permissions.inc','src/fs/ext4_permissions.inc','src/fs/devfs.c','src/fs/tarfs.c']
out=root/'build/permissions-phase5';out.mkdir(parents=True,exist_ok=True)
(out/'syscall-audit.json').write_text(json.dumps({'scope':'reviewed finite source/indirect authority inventory; dynamic coverage separate',
 'syscalls':records,'sources':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in paths},'unclassified':[]},indent=2)+'\n')
print(f'PASS {len(cases)} syscall cases match ABI; every case classified with indirect authority; zero unclassified')
