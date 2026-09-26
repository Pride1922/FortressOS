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

#endif /* SHELL_REDIR_H */
