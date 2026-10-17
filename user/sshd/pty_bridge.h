#ifndef FORTRESS_SSH_PTY_BRIDGE_H
#define FORTRESS_SSH_PTY_BRIDGE_H

#include "types.h"
#include "sshbuf.h"
#include "channel.h"
#include "terminal.h"
#include "syscall_abi.h"

typedef struct {
    int master_fd;
    int slave_fd;
    int64_t child_pid;
    bool child_running;
    bool child_exited;
    int exit_status;
    uint32_t channel_id;
    uint16_t cols;
    uint16_t rows;
    bool pty_allocated;
} pty_bridge_t;

#ifndef __FORTRESS__
typedef long (*pty_bridge_syscall_fn)(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                      uintptr_t d, uintptr_t e, uintptr_t f);
void pty_bridge_set_syscall_hook(pty_bridge_syscall_fn fn);
#endif

/* Initialize bridge context with channel ID and initial window dimensions */
void pty_bridge_init(pty_bridge_t *b, uint32_t channel_id, uint16_t cols, uint16_t rows);

/* Open PTY pair via SYS_PTY_PAIR */
int pty_bridge_open(pty_bridge_t *b);

/* Update terminal dimensions on PTY master */
int pty_bridge_set_winsize(pty_bridge_t *b, uint16_t cols, uint16_t rows);

/* Spawn child shell using SPAWN_SETSID | SPAWN_STAGED and zero-race PTY_SET_SESSION */
int pty_bridge_spawn(pty_bridge_t *b, const char *path, char *const argv[],
                     char *const envp[], const char *cwd);

/* Non-blocking step:
 * 1. Checks child process exit status via WNOHANG.
 * 2. Transfers pending client data from channel->in_data to master_fd.
 * 3. Reads pending output from master_fd and queues SSH_MSG_CHANNEL_DATA packets.
 * 4. Handles EOF and close conditions cleanly.
 * Sets *out_active to true if any I/O or state transition occurred.
 */
int pty_bridge_step(pty_bridge_t *b, ssh_channel_mgr_t *mgr,
                    sshbuf_t *out_packets, bool *out_active);

/* 20ms bounded wait via SYS_NANOSLEEP */
void pty_bridge_sleep_bounded(uint32_t timeout_ms);

/* Clean up descriptors and terminate child if still running */
void pty_bridge_cleanup(pty_bridge_t *b);

#endif /* FORTRESS_SSH_PTY_BRIDGE_H */
