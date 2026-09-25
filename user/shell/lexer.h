#ifndef SHELL_LEXER_H
#define SHELL_LEXER_H

#include "types.h"
#include "lineedit.h"

enum token_type {
    TOK_EOF = 0,
    TOK_WORD,
    TOK_SEMI,    /* ; or newline */
    TOK_AND,     /* && */
    TOK_OR,      /* || */
    TOK_BANG     /* ! */
};

enum lex_status {
    LEX_OK = 0,
    LEX_INCOMPLETE_SQUOTE,
    LEX_INCOMPLETE_DQUOTE,
    LEX_INCOMPLETE_BACKSLASH,
    LEX_INCOMPLETE_OP,
    LEX_ERROR
};

typedef struct {
    enum token_type type;
    const char *value;  /* points into lexer word buffer */
    size_t len;
} token_t;

typedef struct {
    const char *src;
    size_t pos;
    char word_buf[LINE_CAP];
    size_t word_len;
    enum lex_status status;
} lexer_t;

void lexer_init(lexer_t *lex, const char *src);
enum token_type lexer_next(lexer_t *lex, token_t *tok);
enum lex_status lexer_check_incomplete(const char *src);

#endif /* SHELL_LEXER_H */
