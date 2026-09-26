#ifndef SHELL_PARSER_H
#define SHELL_PARSER_H

#include "types.h"
#include "lineedit.h"
#include "lexer.h"

#define MAX_ARGS 64
#define MAX_CMDS 32
#define MAX_REDIRS 16
#define MAX_PIPE_STAGES 8

enum cmd_op {
    CMD_OP_NONE = 0,
    CMD_OP_SEMI,    /* ; or \n */
    CMD_OP_AND,     /* && */
    CMD_OP_OR,      /* || */
    CMD_OP_PIPE     /* stdout pipes to next command; grouping belongs to Phase 4 */
};

typedef struct {
    uint8_t redir_op;           /* REDIR_IN, REDIR_OUT, REDIR_APP, REDIR_DUP_OUT, REDIR_DUP_IN, REDIR_CLOSE */
    int8_t  redir_fd;           /* target fd: 0, 1, 2, ... */
    int8_t  redir_dup_fd;       /* source fd for dup: 0, 1, 2, ... or -1 */
    const char *target;         /* filename for IN/OUT/APP (points into pool), or NULL */
    const uint8_t *quote_flags; /* quote flags for target */
    bool has_quotes;
} redir_t;

typedef struct {
    char *argv[MAX_ARGS + 1];
    const uint8_t *quote_flags[MAX_ARGS + 1];
    bool has_quotes[MAX_ARGS + 1];
    int argc;
    redir_t redirs[MAX_REDIRS];
    int redir_count;
    bool negate;          /* ! prefix */
    enum cmd_op next_op;  /* op connecting to next command */
} parse_cmd_t;

typedef struct {
    parse_cmd_t cmds[MAX_CMDS];
    int cmd_count;
    char pool[LINE_CAP * 2];
    uint8_t qpool[LINE_CAP * 2];
    size_t pool_used;
    enum lex_status status;
    const char *error_msg;
} parse_tree_t;

enum parse_result {
    PARSE_OK = 0,
    PARSE_INCOMPLETE,
    PARSE_SYNTAX_ERROR,
    PARSE_EMPTY
};

enum parse_result parser_parse(const char *src, parse_tree_t *tree);

#endif /* SHELL_PARSER_H */
