#include "parser.h"

static lexer_t parse_lex;

enum parse_result parser_parse(const char *src, parse_tree_t *tree) {
    if (!src || !tree) return PARSE_EMPTY;

    tree->cmd_count = 0;
    tree->pool_used = 0;
    tree->status = LEX_OK;
    tree->error_msg = 0;

    enum lex_status inc = lexer_check_incomplete(src);
    if (inc != LEX_OK) {
        tree->status = inc;
        return PARSE_INCOMPLETE;
    }

    token_t tok;
    lexer_init(&parse_lex, src);

    enum token_type type = lexer_next(&parse_lex, &tok);
    while (type != TOK_EOF) {
        if (type == TOK_SEMI) {
            type = lexer_next(&parse_lex, &tok);
            continue;
        }

        if (type == TOK_AND || type == TOK_OR || type == TOK_PIPE) {
            tree->error_msg = "syntax error near unexpected token";
            return PARSE_SYNTAX_ERROR;
        }

        if (tree->cmd_count >= MAX_CMDS) {
            tree->error_msg = "too many chained commands";
            return PARSE_SYNTAX_ERROR;
        }

        parse_cmd_t *cmd = &tree->cmds[tree->cmd_count];
        cmd->argc = 0;
        cmd->redir_count = 0;
        cmd->negate = false;
        cmd->next_op = CMD_OP_NONE;

        if (type == TOK_BANG) {
            cmd->negate = true;
            type = lexer_next(&parse_lex, &tok);
            if (type != TOK_WORD && type != TOK_REDIR) {
                tree->error_msg = "syntax error: expected command after '!'";
                return PARSE_SYNTAX_ERROR;
            }
        }

        if (type != TOK_WORD && type != TOK_REDIR) {
            tree->error_msg = "syntax error near unexpected token";
            return PARSE_SYNTAX_ERROR;
        }

        while (type == TOK_WORD || type == TOK_REDIR) {
            if (type == TOK_REDIR) {
                if (cmd->redir_count >= MAX_REDIRS) {
                    tree->error_msg = "too many redirections";
                    return PARSE_SYNTAX_ERROR;
                }
                redir_t *r = &cmd->redirs[cmd->redir_count];
                r->redir_op = tok.redir_op;
                r->redir_fd = tok.redir_fd;
                r->redir_dup_fd = tok.redir_dup_fd;
                r->target = NULL;
                r->quote_flags = NULL;
                r->has_quotes = false;
                if (r->redir_fd < 0 || r->redir_fd >= 32 || r->redir_dup_fd >= 32) {
                    tree->error_msg = "file descriptor out of range";
                    return PARSE_SYNTAX_ERROR;
                }

                if (r->redir_op == REDIR_IN || r->redir_op == REDIR_OUT || r->redir_op == REDIR_APP) {
                    type = lexer_next(&parse_lex, &tok);
                    if (type == TOK_EOF) {
                        tree->status = LEX_INCOMPLETE_OP;
                        return PARSE_INCOMPLETE;
                    }
                    if (type != TOK_WORD) {
                        tree->error_msg = "syntax error near unexpected token";
                        return PARSE_SYNTAX_ERROR;
                    }
                    size_t wlen = tok.len;
                    if (tree->pool_used + wlen + 1 > sizeof(tree->pool)) {
                        tree->error_msg = "redirection target buffer exhausted";
                        return PARSE_SYNTAX_ERROR;
                    }
                    char *dest = &tree->pool[tree->pool_used];
                    uint8_t *qdest = &tree->qpool[tree->pool_used];
                    for (size_t k = 0; k < wlen; k++) {
                        dest[k] = tok.value[k];
                        qdest[k] = tok.quote_flags ? tok.quote_flags[k] : 0;
                    }
                    dest[wlen] = '\0';
                    qdest[wlen] = 0;
                    tree->pool_used += wlen + 1;

                    r->target = dest;
                    r->quote_flags = qdest;
                    r->has_quotes = tok.has_quotes;
                } else if (r->redir_op == REDIR_DUP_OUT || r->redir_op == REDIR_DUP_IN) {
                    if (r->redir_dup_fd < 0) {
                        type = lexer_next(&parse_lex, &tok);
                        if (type == TOK_EOF) {
                            tree->status = LEX_INCOMPLETE_OP;
                            return PARSE_INCOMPLETE;
                        }
                        if (type != TOK_WORD) {
                            tree->error_msg = "syntax error near unexpected token";
                            return PARSE_SYNTAX_ERROR;
                        }
                        if (tok.len == 1 && tok.value[0] == '-') {
                            r->redir_op = REDIR_CLOSE;
                            r->redir_dup_fd = -1;
                        } else {
                            int dfd = 0;
                            for (size_t k = 0; k < tok.len; k++) {
                                if (tok.value[k] < '0' || tok.value[k] > '9') {
                                    tree->error_msg = "syntax error: expected file descriptor number";
                                    return PARSE_SYNTAX_ERROR;
                                }
                                int digit = tok.value[k] - '0';
                                if (dfd > (31 - digit) / 10) {
                                    tree->error_msg = "file descriptor out of range";
                                    return PARSE_SYNTAX_ERROR;
                                }
                                dfd = dfd * 10 + digit;
                            }
                            if (dfd >= 32) {
                                tree->error_msg = "file descriptor out of range";
                                return PARSE_SYNTAX_ERROR;
                            }
                            r->redir_dup_fd = (int8_t)dfd;
                        }
                    }
                }
                cmd->redir_count++;
                type = lexer_next(&parse_lex, &tok);
                continue;
            }

            if (cmd->argc >= MAX_ARGS) {
                tree->error_msg = "too many arguments";
                return PARSE_SYNTAX_ERROR;
            }

            size_t wlen = tok.len;
            if (tree->pool_used + wlen + 1 > sizeof(tree->pool)) {
                tree->error_msg = "command argument buffer exhausted";
                return PARSE_SYNTAX_ERROR;
            }

            char *dest = &tree->pool[tree->pool_used];
            uint8_t *qdest = &tree->qpool[tree->pool_used];
            for (size_t k = 0; k < wlen; k++) {
                dest[k] = tok.value[k];
                qdest[k] = tok.quote_flags ? tok.quote_flags[k] : 0;
            }
            dest[wlen] = '\0';
            qdest[wlen] = 0;
            tree->pool_used += wlen + 1;

            cmd->quote_flags[cmd->argc] = qdest;
            cmd->has_quotes[cmd->argc] = tok.has_quotes;
            cmd->argv[cmd->argc++] = dest;
            type = lexer_next(&parse_lex, &tok);
        }
        cmd->argv[cmd->argc] = 0;
        cmd->quote_flags[cmd->argc] = 0;

        if (type == TOK_PIPE) {
            cmd->next_op = CMD_OP_PIPE;
            type = lexer_next(&parse_lex, &tok);
            if (type != TOK_WORD && type != TOK_REDIR && type != TOK_BANG) {
                tree->error_msg = "syntax error: expected command after '|'";
                return PARSE_SYNTAX_ERROR;
            }
        } else if (type == TOK_SEMI) {
            cmd->next_op = CMD_OP_SEMI;
            type = lexer_next(&parse_lex, &tok);
        } else if (type == TOK_AND) {
            cmd->next_op = CMD_OP_AND;
            type = lexer_next(&parse_lex, &tok);
            if (type == TOK_EOF) {
                tree->status = LEX_INCOMPLETE_OP;
                return PARSE_INCOMPLETE;
            }
        } else if (type == TOK_OR) {
            cmd->next_op = CMD_OP_OR;
            type = lexer_next(&parse_lex, &tok);
            if (type == TOK_EOF) {
                tree->status = LEX_INCOMPLETE_OP;
                return PARSE_INCOMPLETE;
            }
        } else if (type == TOK_EOF) {
            cmd->next_op = CMD_OP_NONE;
        } else {
            tree->error_msg = "syntax error near unexpected token";
            return PARSE_SYNTAX_ERROR;
        }

        tree->cmd_count++;
    }

    if (parse_lex.status != LEX_OK) {
        tree->status = parse_lex.status;
        return PARSE_INCOMPLETE;
    }

    if (tree->cmd_count == 0) return PARSE_EMPTY;
    /* N stages have N-1 consecutive PIPE markers. Chain operators reset the
     * count; MAX_CMDS independently bounds the entire flat command array. */
    int pipe_run = 0;
    for (int i = 0; i < tree->cmd_count; i++) {
        if (tree->cmds[i].next_op == CMD_OP_PIPE) {
            if (++pipe_run >= MAX_PIPE_STAGES) {
                tree->error_msg = "pipeline too long (maximum 8 stages)";
                return PARSE_SYNTAX_ERROR;
            }
        } else {
            pipe_run = 0;
        }
    }
    return PARSE_OK;
}

int parser_execution_guard(const parse_tree_t *tree, long (*diagnostic)(const char *)) {
    for (int i = 0; i < tree->cmd_count; i++) {
        if (tree->cmds[i].next_op == CMD_OP_PIPE) {
            diagnostic("pipeline: not yet supported\n");
            return 1;
        }
    }
    return 0;
}
