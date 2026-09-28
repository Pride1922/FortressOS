#ifndef SHELL_PIPELINE_H
#define SHELL_PIPELINE_H
#include "parser.h"

/* Single command callback retains its existing negation semantics.
 * Static preparation storage: shell execution is deliberately non-reentrant.
 * Blocking pipe peers must run on the BSP until S7 Phase 6. */
int execute_command_list(parse_tree_t *tree, int status,
                         int (*single)(parse_cmd_t *, int));

/* Job-aware command list execution: routes through the job table when
 * tree->background is set, using staged launch and terminal handoff.
 * cmd_text is the original command line for job display. */
int execute_command_list_job(parse_tree_t *tree, int status,
                             int (*single)(parse_cmd_t *, int),
                             const char *cmd_text);
#endif
