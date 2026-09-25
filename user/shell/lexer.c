#include "lexer.h"

void lexer_init(lexer_t *lex, const char *src) {
    lex->src = src ? src : "";
    lex->pos = 0;
    lex->word_len = 0;
    lex->word_buf[0] = '\0';
    lex->status = LEX_OK;
}

static inline bool is_whitespace(char c) {
    return c == ' ' || c == '\t';
}

static inline bool is_operator_char(char c) {
    return c == ';' || c == '&' || c == '|' || c == '\n';
}

enum token_type lexer_next(lexer_t *lex, token_t *tok) {
    const char *s = lex->src;
    size_t i = lex->pos;

    tok->type = TOK_EOF;
    tok->value = "";
    tok->len = 0;

    /* Skip leading whitespace and comments */
    for (;;) {
        while (s[i] && is_whitespace(s[i])) i++;

        if (s[i] == '#') {
            /* Comment at token start outside quotes: skip until newline or EOF */
            while (s[i] && s[i] != '\n') i++;
            continue;
        }

        /* Check for escaped newline outside quotes: line continuation */
        if (s[i] == '\\' && s[i + 1] == '\n') {
            i += 2;
            continue;
        }

        break;
    }

    if (!s[i]) {
        lex->pos = i;
        return TOK_EOF;
    }

    /* Single-character / double-character operators */
    if (s[i] == ';') {
        lex->pos = i + 1;
        tok->type = TOK_SEMI;
        tok->value = ";";
        tok->len = 1;
        return TOK_SEMI;
    }

    if (s[i] == '\n') {
        lex->pos = i + 1;
        tok->type = TOK_SEMI;
        tok->value = "\n";
        tok->len = 1;
        return TOK_SEMI;
    }

    if (s[i] == '&' && s[i + 1] == '&') {
        lex->pos = i + 2;
        tok->type = TOK_AND;
        tok->value = "&&";
        tok->len = 2;
        return TOK_AND;
    }

    if (s[i] == '|' && s[i + 1] == '|') {
        lex->pos = i + 2;
        tok->type = TOK_OR;
        tok->value = "||";
        tok->len = 2;
        return TOK_OR;
    }

    if (s[i] == '!' && (is_whitespace(s[i + 1]) || is_operator_char(s[i + 1]) || !s[i + 1])) {
        lex->pos = i + 1;
        tok->type = TOK_BANG;
        tok->value = "!";
        tok->len = 1;
        return TOK_BANG;
    }

    /* Read a TOK_WORD */
    lex->word_len = 0;
    lex->has_quotes = false;
    bool has_content = false;

    while (s[i]) {
        if (is_whitespace(s[i]) || is_operator_char(s[i])) {
            break;
        }

        if (s[i] == '\'') {
            /* Single quotes: preserve literal characters */
            has_content = true;
            lex->has_quotes = true;
            i++;
            while (s[i] && s[i] != '\'') {
                if (lex->word_len + 1 < sizeof(lex->word_buf)) {
                    lex->quote_flags[lex->word_len] = QUOTE_SINGLE;
                    lex->word_buf[lex->word_len++] = s[i];
                }
                i++;
            }
            if (!s[i]) {
                lex->status = LEX_INCOMPLETE_SQUOTE;
                lex->pos = i;
                return TOK_EOF;
            }
            i++; /* skip closing ' */
            continue;
        }

        if (s[i] == '"') {
            /* Double quotes: preserve characters, allow escapes */
            has_content = true;
            lex->has_quotes = true;
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\') {
                    i++;
                    if (!s[i]) {
                        lex->status = LEX_INCOMPLETE_DQUOTE;
                        lex->pos = i;
                        return TOK_EOF;
                    }
                    char esc = s[i++];
                    char out_c = esc;
                    if (esc == 'n') out_c = '\n';
                    else if (esc == 't') out_c = '\t';
                    /* other escapes \", \\, \$, etc. are literal */
                    if (lex->word_len + 1 < sizeof(lex->word_buf)) {
                        lex->quote_flags[lex->word_len] = QUOTE_DOUBLE;
                        lex->word_buf[lex->word_len++] = out_c;
                    }
                    continue;
                }
                if (lex->word_len + 1 < sizeof(lex->word_buf)) {
                    lex->quote_flags[lex->word_len] = QUOTE_DOUBLE;
                    lex->word_buf[lex->word_len++] = s[i];
                }
                i++;
            }
            if (!s[i]) {
                lex->status = LEX_INCOMPLETE_DQUOTE;
                lex->pos = i;
                return TOK_EOF;
            }
            i++; /* skip closing " */
            continue;
        }

        if (s[i] == '\\') {
            /* Backslash escape outside quotes */
            has_content = true;
            lex->has_quotes = true;
            i++;
            if (!s[i]) {
                lex->status = LEX_INCOMPLETE_BACKSLASH;
                lex->pos = i;
                return TOK_EOF;
            }
            if (s[i] == '\n') {
                i++; /* line continuation */
                continue;
            }
            if (lex->word_len + 1 < sizeof(lex->word_buf)) {
                lex->quote_flags[lex->word_len] = QUOTE_ESCAPED;
                lex->word_buf[lex->word_len++] = s[i++];
            } else {
                i++;
            }
            continue;
        }

        /* Ordinary character */
        has_content = true;
        if (lex->word_len + 1 < sizeof(lex->word_buf)) {
            lex->quote_flags[lex->word_len] = QUOTE_NONE;
            lex->word_buf[lex->word_len++] = s[i++];
        } else {
            i++;
        }
    }

    if (has_content || lex->word_len == 0) {
        lex->word_buf[lex->word_len] = '\0';
        lex->quote_flags[lex->word_len] = 0;
        lex->pos = i;
        tok->type = TOK_WORD;
        tok->value = lex->word_buf;
        tok->quote_flags = lex->quote_flags;
        tok->len = lex->word_len;
        tok->has_quotes = lex->has_quotes;
        return TOK_WORD;
    }

    lex->pos = i;
    return TOK_EOF;
}

static lexer_t inc_lex;

enum lex_status lexer_check_incomplete(const char *src) {
    if (!src) return LEX_OK;
    token_t tok;
    lexer_init(&inc_lex, src);
    enum token_type last_type = TOK_EOF;

    while (lexer_next(&inc_lex, &tok) != TOK_EOF) {
        last_type = tok.type;
    }

    if (inc_lex.status != LEX_OK) {
        return inc_lex.status;
    }

    if (last_type == TOK_AND || last_type == TOK_OR) {
        return LEX_INCOMPLETE_OP;
    }

    /* Check trailing backslash that might not have been caught */
    size_t len = 0;
    while (src[len]) len++;
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    if (len && src[len - 1] == '\\') {
        /* Check if backslash was escaped by an odd number of backslashes */
        size_t bscount = 0;
        size_t k = len;
        while (k && src[k - 1] == '\\') { bscount++; k--; }
        if (bscount % 2 != 0) return LEX_INCOMPLETE_BACKSLASH;
    }

    return LEX_OK;
}
