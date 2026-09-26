#ifndef SHELL_REDIR_H
#define SHELL_REDIR_H

#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "parser.h"

/*
 * Convert AST redir_t records into kernel spawn_fd_action_t records for SYS_SPAWN_EXT.
 * Expands target filenames via expand_redir_target.
 *
 * Returns 0 on success, or negative on error (with diagnostic printed to stderr):
 *  -1: Ambiguous redirect or expansion error
 *  -2: Too many redirections (> MAX_SPAWN_ACTIONS)
 *  -3: Invalid descriptor or syntax
 */
int redir_build_spawn_actions(const redir_t *redirs, int redir_count, int64_t last_status,
                              spawn_fd_action_t *out_actions, uint32_t *out_action_count,
                              char target_paths[MAX_SPAWN_ACTIONS][VFS_MAX_PATH]);

typedef struct {
    int orig_fd;     /* Target descriptor that was redirected (0..31) */
    int saved_fd;    /* Duplicate copy of orig_fd (with CLOEXEC), or -1 if orig_fd was closed */
    bool was_open;   /* True if orig_fd was open before redirections */
} redir_save_t;

typedef struct {
    redir_save_t saved[MAX_SPAWN_ACTIONS];
    int count;
} redir_scope_t;

/*
 * Apply redirections within the current shell process for builtins.
 * Saves existing file descriptors into high slots with CLOEXEC before modifying them.
 * On failure, restores any already-modified descriptors and returns non-zero.
 */
int redir_apply_parent(const redir_t *redirs, int redir_count, int64_t last_status,
                       redir_scope_t *scope);

/*
 * Restore all saved descriptors to their original state and close temporary saved copies.
 */
void redir_restore_parent(redir_scope_t *scope);

#endif /* SHELL_REDIR_H */
