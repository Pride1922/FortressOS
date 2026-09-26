#ifndef SHELL_PIPELINE_H
#define SHELL_PIPELINE_H
#include "parser.h"

/* Single command callback retains its existing negation semantics.
 * Static preparation storage: shell execution is deliberately non-reentrant.
 * Blocking pipe peers must run on the BSP until S7 Phase 6. */
int execute_command_list(parse_tree_t *tree, int status,
                         int (*single)(parse_cmd_t *, int));
#endif
