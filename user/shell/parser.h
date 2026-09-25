#ifndef SHELL_PARSER_H
#define SHELL_PARSER_H

#include "types.h"
#include "lineedit.h"
#include "lexer.h"

#define MAX_ARGS 64
#define MAX_CMDS 32

enum cmd_op {
    CMD_OP_NONE = 0,
    CMD_OP_SEMI,    /* ; or \n */
    CMD_OP_AND,     /* && */
    CMD_OP_OR       /* || */
};

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    bool negate;          /* ! prefix */
    enum cmd_op next_op;  /* op connecting to next command */
} parse_cmd_t;

typedef struct {
    parse_cmd_t cmds[MAX_CMDS];
    int cmd_count;
    char pool[LINE_CAP * 2];
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
