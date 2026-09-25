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

        if (type == TOK_AND || type == TOK_OR) {
            tree->error_msg = "syntax error near unexpected token";
            return PARSE_SYNTAX_ERROR;
        }

        if (tree->cmd_count >= MAX_CMDS) {
            tree->error_msg = "too many chained commands";
            return PARSE_SYNTAX_ERROR;
        }

        parse_cmd_t *cmd = &tree->cmds[tree->cmd_count];
        cmd->argc = 0;
        cmd->negate = false;
        cmd->next_op = CMD_OP_NONE;

        if (type == TOK_BANG) {
            cmd->negate = true;
            type = lexer_next(&parse_lex, &tok);
            if (type != TOK_WORD) {
                tree->error_msg = "syntax error: expected command after '!'";
                return PARSE_SYNTAX_ERROR;
            }
        }

        if (type != TOK_WORD) {
            tree->error_msg = "syntax error near unexpected token";
            return PARSE_SYNTAX_ERROR;
        }

        while (type == TOK_WORD) {
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

        if (type == TOK_SEMI) {
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
    return PARSE_OK;
}
