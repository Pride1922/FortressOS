#ifndef SHELL_PIPELINE_H
#define SHELL_PIPELINE_H
#include "parser.h"
#include "syscall_abi.h"
void pipeline_set_interactive(bool enabled);
void pipeline_set_cancel_flag(volatile unsigned *flag);
void pipeline_set_stop_flag(unsigned *flag);
unsigned pipeline_last_signal(void);

typedef struct {
    int first;
    enum cmd_op incoming;
    bool waiting;
} pipeline_cursor_t;
int execute_command_list_resume(parse_tree_t *tree,int status,
    int (*single)(parse_cmd_t *,int),const char *text,pipeline_cursor_t *cursor);

/* Single command callback retains its existing negation semantics.
 * Preparation scratch is not reentrant. A source yield unwinds completely
 * before reuse; the caller owns its AST and command-list cursor.
 * Blocking pipe peers must run on the BSP until S7 Phase 6. */
int execute_command_list(parse_tree_t *tree, int status,
                         int (*single)(parse_cmd_t *, int));

/* Job-aware command list execution: routes through the job table when
 * tree->background is set, using staged launch and terminal handoff.
 * cmd_text is the original command line for job display. */
int execute_command_list_job(parse_tree_t *tree, int status,
                             int (*single)(parse_cmd_t *, int),
                             const char *cmd_text);
/* staged=true consumes the masked launch window and restores old_mask.
 * fg uses staged=false and does not alter the caller's mask. */
int pipeline_foreground(int slot, bool staged, uint64_t old_mask);
int pipeline_run_program(const char *path, const char *const *argv,
                         const char *const *envp, const spawn_fd_action_t *actions,
                         uint32_t count);
#endif
