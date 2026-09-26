#include "redir.h"
#include "expand.h"
#include "io.h"
#include "lexer.h"
#include "parser.h"
#include "vfs.h"

int redir_build_spawn_actions(const redir_t *redirs, int redir_count, int64_t last_status,
                              spawn_fd_action_t *out_actions, uint32_t *out_action_count,
                              char target_paths[MAX_SPAWN_ACTIONS][VFS_MAX_PATH]) {
    if (!out_actions || !out_action_count || !target_paths) return -1;
    *out_action_count = 0;
    if (redir_count == 0) return 0;
    if (redir_count > MAX_SPAWN_ACTIONS) {
        puts_err("Too many redirections (max 16).\n");
        return -2;
    }

    for (int i = 0; i < redir_count; i++) {
        const redir_t *r = &redirs[i];
        spawn_fd_action_t *act = &out_actions[i];
        act->type = 0;
        act->dst_fd = r->redir_fd;
        act->src_fd = -1;
        act->flags = 0;
        act->mode = 0;
        act->reserved = 0;
        act->path = 0;

        if (r->redir_fd < 0 || r->redir_fd >= 32) {
            puts_err("Bad file descriptor.\n");
            return -3;
        }

        switch (r->redir_op) {
            case REDIR_IN: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, target_paths[i], VFS_MAX_PATH) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    return -1;
                }
                act->type = SPAWN_FD_ACTION_OPEN;
                act->dst_fd = r->redir_fd;
                act->flags = VFS_O_RDONLY;
                act->path = (uint64_t)target_paths[i];
                break;
            }
            case REDIR_OUT: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, target_paths[i], VFS_MAX_PATH) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    return -1;
                }
                act->type = SPAWN_FD_ACTION_OPEN;
                act->dst_fd = r->redir_fd;
                act->flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC;
                act->mode = 0644;
                act->path = (uint64_t)target_paths[i];
                break;
            }
            case REDIR_APP: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, target_paths[i], VFS_MAX_PATH) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    return -1;
                }
                act->type = SPAWN_FD_ACTION_OPEN;
                act->dst_fd = r->redir_fd;
                act->flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND;
                act->mode = 0644;
                act->path = (uint64_t)target_paths[i];
                break;
            }
            case REDIR_DUP_OUT:
            case REDIR_DUP_IN: {
                if (r->redir_dup_fd < 0 || r->redir_dup_fd >= 32) {
                    puts_err("Bad file descriptor.\n");
                    return -3;
                }
                act->type = SPAWN_FD_ACTION_DUP2;
                act->dst_fd = r->redir_fd;
                act->src_fd = r->redir_dup_fd;
                break;
            }
            case REDIR_CLOSE: {
                act->type = SPAWN_FD_ACTION_CLOSE;
                act->dst_fd = r->redir_fd;
                break;
            }
            default:
                puts_err("Invalid redirection operator.\n");
                return -3;
        }
    }

    *out_action_count = (uint32_t)redir_count;
    return 0;
}

static char s_parent_target[VFS_MAX_PATH];

static int save_descriptor(int fd, redir_scope_t *scope) {
    if (!scope || fd < 0 || fd >= 32) return -1;
    for (int i = 0; i < scope->count; i++) {
        if (scope->saved[i].orig_fd == fd) {
            return 0; /* Already saved initial state */
        }
    }
    if (scope->count >= MAX_SPAWN_ACTIONS) {
        puts_err("Too many redirections.\n");
        return -1;
    }

    long saved = call(SYS_FCNTL, (uintptr_t)fd, F_DUPFD_CLOEXEC, 20);
    if (saved >= 0) {
        scope->saved[scope->count].orig_fd = fd;
        scope->saved[scope->count].saved_fd = (int)saved;
        scope->saved[scope->count].was_open = true;
        scope->count++;
        return 0;
    } else if (saved == SYSCALL_EBADF) {
        scope->saved[scope->count].orig_fd = fd;
        scope->saved[scope->count].saved_fd = -1;
        scope->saved[scope->count].was_open = false;
        scope->count++;
        return 0;
    } else {
        puts_err("Failed to duplicate file descriptor.\n");
        return -1;
    }
}

void redir_restore_parent(redir_scope_t *scope) {
    if (!scope || scope->count == 0) return;
    for (int i = scope->count - 1; i >= 0; i--) {
        redir_save_t *s = &scope->saved[i];
        if (s->was_open) {
            (void)call(SYS_DUP2, (uintptr_t)s->saved_fd, (uintptr_t)s->orig_fd, 0);
            (void)call(SYS_CLOSE, (uintptr_t)s->saved_fd, 0, 0);
        } else {
            (void)call(SYS_CLOSE, (uintptr_t)s->orig_fd, 0, 0);
        }
    }
    scope->count = 0;
}

int redir_apply_parent(const redir_t *redirs, int redir_count, int64_t last_status,
                       redir_scope_t *scope) {
    if (!scope) return -1;
    scope->count = 0;
    if (redir_count == 0 || !redirs) return 0;
    if (redir_count > MAX_SPAWN_ACTIONS) {
        puts_err("Too many redirections (max 16).\n");
        return -1;
    }

    for (int i = 0; i < redir_count; i++) {
        const redir_t *r = &redirs[i];
        if (r->redir_fd < 0 || r->redir_fd >= 32) {
            puts_err("Bad file descriptor.\n");
            redir_restore_parent(scope);
            return -1;
        }

        if (save_descriptor(r->redir_fd, scope) != 0) {
            redir_restore_parent(scope);
            return -1;
        }

        switch (r->redir_op) {
            case REDIR_IN: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, s_parent_target, sizeof(s_parent_target)) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                long fd = call(SYS_OPEN, (uintptr_t)s_parent_target, VFS_O_RDONLY, 0);
                if (fd < 0) {
                    file_error_err(fd);
                    redir_restore_parent(scope);
                    return -1;
                }
                (void)call(SYS_DUP2, (uintptr_t)fd, (uintptr_t)r->redir_fd, 0);
                (void)call(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                break;
            }
            case REDIR_OUT: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, s_parent_target, sizeof(s_parent_target)) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                long fd = call(SYS_OPEN, (uintptr_t)s_parent_target,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
                if (fd < 0) {
                    file_error_err(fd);
                    redir_restore_parent(scope);
                    return -1;
                }
                (void)call(SYS_DUP2, (uintptr_t)fd, (uintptr_t)r->redir_fd, 0);
                (void)call(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                break;
            }
            case REDIR_APP: {
                if (!r->target) {
                    puts_err("Missing redirection target.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                if (expand_redir_target(r->target, r->quote_flags, r->has_quotes,
                                        last_status, s_parent_target, sizeof(s_parent_target)) != 0) {
                    puts_err("Ambiguous redirect.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                long fd = call(SYS_OPEN, (uintptr_t)s_parent_target,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, 0644);
                if (fd < 0) {
                    file_error_err(fd);
                    redir_restore_parent(scope);
                    return -1;
                }
                (void)call(SYS_DUP2, (uintptr_t)fd, (uintptr_t)r->redir_fd, 0);
                (void)call(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                break;
            }
            case REDIR_DUP_OUT:
            case REDIR_DUP_IN: {
                if (r->redir_dup_fd < 0 || r->redir_dup_fd >= 32) {
                    puts_err("Bad file descriptor.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                long res = call(SYS_DUP2, (uintptr_t)r->redir_dup_fd, (uintptr_t)r->redir_fd, 0);
                if (res < 0) {
                    puts_err("Bad file descriptor.\n");
                    redir_restore_parent(scope);
                    return -1;
                }
                break;
            }
            case REDIR_CLOSE: {
                (void)call(SYS_CLOSE, (uintptr_t)r->redir_fd, 0, 0);
                break;
            }
            default:
                puts_err("Invalid redirection operator.\n");
                redir_restore_parent(scope);
                return -1;
        }
    }
    return 0;
}
