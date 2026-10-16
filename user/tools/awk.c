#include "common.h"

#define AWK_LINE_CAP       65536
#define AWK_MAX_FIELDS     64
#define AWK_FIELD_STR_CAP  1024
#define AWK_MAX_TOKENS     64
#define AWK_MAX_NODES      256
#define AWK_MAX_RULES      32
#define AWK_MAX_STMTS      16
#define AWK_MAX_ARGS       16
#define AWK_MAX_FILES      64

/* --------------------------------------------------------------------------
 * Regex Types & Engine (reusing grep's bitmask NFA)
 * -------------------------------------------------------------------------- */

typedef enum {
    TOK_LITERAL,
    TOK_DOT,
    TOK_CLASS,
    TOK_NCLASS
} regex_token_kind_t;

typedef struct {
    regex_token_kind_t kind;
    char ch;
    uint8_t cls[32]; /* 256-bit set */
    bool star;
} regex_token_t;

typedef struct {
    regex_token_t tokens[AWK_MAX_TOKENS];
    int num_tokens;
    bool anchor_start;
    bool anchor_end;
    bool ignore_case;
} regex_t;

static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static inline void set_bit(uint8_t *set, unsigned char b) {
    set[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static inline bool test_bit(const uint8_t *set, unsigned char b) {
    return (set[b >> 3] & (1u << (b & 7))) != 0;
}

static inline void set_bit_case(uint8_t *set, unsigned char b, bool icase) {
    set_bit(set, b);
    if (icase) {
        if (b >= 'a' && b <= 'z') set_bit(set, (unsigned char)(b - ('a' - 'A')));
        else if (b >= 'A' && b <= 'Z') set_bit(set, (unsigned char)(b + ('a' - 'A')));
    }
}

static inline bool match_token(const regex_token_t *tok, unsigned char c, bool icase) {
    if (c == '\0' || c == '\n' || c == '\r') return false;
    switch (tok->kind) {
    case TOK_DOT:
        return true;
    case TOK_LITERAL: {
        char c1 = tok->ch;
        char c2 = (char)c;
        if (icase) {
            c1 = to_lower(c1);
            c2 = to_lower(c2);
        }
        return c1 == c2;
    }
    case TOK_CLASS:
        return test_bit(tok->cls, c);
    case TOK_NCLASS:
        return !test_bit(tok->cls, c);
    }
    return false;
}

static inline uint64_t nfa_closure(const regex_t *re, uint64_t states) {
    for (int i = 0; i < re->num_tokens; i++) {
        if ((states & (1ULL << i)) && re->tokens[i].star) {
            states |= (1ULL << (i + 1));
        }
    }
    return states;
}

static inline uint64_t nfa_step(const regex_t *re, uint64_t states, unsigned char c) {
    uint64_t next_states = 0;
    for (int i = 0; i < re->num_tokens; i++) {
        if (states & (1ULL << i)) {
            if (match_token(&re->tokens[i], c, re->ignore_case)) {
                if (re->tokens[i].star) {
                    next_states |= (1ULL << i) | (1ULL << (i + 1));
                } else {
                    next_states |= (1ULL << (i + 1));
                }
            }
        }
    }
    return nfa_closure(re, next_states);
}

static bool compile_pattern(regex_t *re, const char *pattern, size_t pat_len, bool icase) {
    for (size_t i = 0; i < sizeof(*re); i++) ((char *)re)[i] = 0;
    re->ignore_case = icase;
    re->num_tokens = 0;

    if (pat_len == 0) return true;

    const char *p = pattern;
    const char *end = pattern + pat_len;

    if (*p == '^') {
        re->anchor_start = true;
        p++;
    }

    while (p < end) {
        if (*p == '$' && p + 1 == end) {
            re->anchor_end = true;
            p++;
            break;
        }

        if (re->num_tokens >= AWK_MAX_TOKENS) return false;
        regex_token_t *tok = &re->tokens[re->num_tokens];

        if (*p == '\\' && p + 1 < end) {
            char next = p[1];
            if (next == 'n') {
                tok->kind = TOK_LITERAL;
                tok->ch = '\n';
                tok->star = false;
                re->num_tokens++;
                p += 2;
            } else if (next == 't') {
                tok->kind = TOK_LITERAL;
                tok->ch = '\t';
                tok->star = false;
                re->num_tokens++;
                p += 2;
            } else {
                tok->kind = TOK_LITERAL;
                tok->ch = next;
                tok->star = false;
                re->num_tokens++;
                p += 2;
            }
        } else if (*p == '.') {
            tok->kind = TOK_DOT;
            tok->star = false;
            re->num_tokens++;
            p++;
        } else if (*p == '[') {
            p++;
            bool negated = false;
            if (p < end && *p == '^') {
                negated = true;
                p++;
            }
            tok->kind = negated ? TOK_NCLASS : TOK_CLASS;
            tok->star = false;

            if (p < end && *p == ']') {
                set_bit_case(tok->cls, (unsigned char)*p, icase);
                p++;
            }

            while (p < end && *p != ']') {
                if (p + 2 < end && p[1] == '-' && p[2] != ']') {
                    unsigned char start = (unsigned char)p[0];
                    unsigned char stop = (unsigned char)p[2];
                    if (start > stop) {
                        unsigned char tmp = start; start = stop; stop = tmp;
                    }
                    for (unsigned i = start; i <= stop; i++) {
                        set_bit_case(tok->cls, (unsigned char)i, icase);
                    }
                    p += 3;
                } else {
                    set_bit_case(tok->cls, (unsigned char)*p, icase);
                    p++;
                }
            }
            if (p < end && *p == ']') p++;
            re->num_tokens++;
        } else if (*p == '*') {
            if (re->num_tokens > 0) {
                re->tokens[re->num_tokens - 1].star = true;
            }
            while (p < end && *p == '*') p++;
        } else {
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            re->num_tokens++;
        }
    }

    return true;
}

static bool regex_matches(const regex_t *re, const char *line, size_t line_len) {
    if (re->num_tokens == 0) {
        if (re->anchor_start && re->anchor_end) return line_len == 0;
        return true;
    }

    uint64_t accept_mask = (1ULL << re->num_tokens);
    size_t min_pos = 0;
    size_t max_pos = re->anchor_start ? 0 : line_len;

    for (size_t pos = min_pos; pos <= max_pos; pos++) {
        uint64_t states = nfa_closure(re, 1ULL << 0);
        if (states & accept_mask) {
            if (!re->anchor_end || pos == line_len) return true;
        }

        for (size_t j = pos; j < line_len; j++) {
            states = nfa_step(re, states, (unsigned char)line[j]);
            if (states & accept_mask) {
                if (!re->anchor_end || (j + 1 == line_len)) return true;
            }
            if (states == 0) break;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Numbers & Dual-Type Values
 * -------------------------------------------------------------------------- */

typedef struct {
    int64_t num;
    const char *str;
    size_t len;
    bool is_num;
} awk_val_t;

static char s_eval_buf[16][128];
static int s_eval_buf_idx = 0;
static char s_concat_buf[4][AWK_FIELD_STR_CAP];
static int s_concat_idx = 0;

static size_t format_i64(char *buf, size_t cap, int64_t val) {
    if (cap == 0) return 0;
    if (val < 0) {
        if (cap < 2) return 0;
        buf[0] = '-';
        uint64_t u = (uint64_t)(-(val + 1)) + 1;
        return 1 + tool_format_u64(buf + 1, u);
    }
    return tool_format_u64(buf, (uint64_t)val);
}

static bool try_parse_i64(const char *s, int64_t *out_val) {
    if (!s || !*s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') { s++; }
    if (!*s || (*s < '0' || *s > '9')) return false;

    uint64_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (uint64_t)(*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') return false;

    *out_val = neg ? -((int64_t)val) : (int64_t)val;
    return true;
}

static bool is_numeric_string(const char *s) {
    int64_t dummy;
    return try_parse_i64(s, &dummy);
}

static bool is_truthy(const awk_val_t *v) {
    if (v->is_num) {
        return v->num != 0;
    }
    int64_t d;
    if (try_parse_i64(v->str, &d)) {
        return d != 0;
    }
    return v->len > 0 && v->str[0] != '\0';
}

static int compare_vals(const awk_val_t *v1, const awk_val_t *v2) {
    if (is_numeric_string(v1->str) && is_numeric_string(v2->str)) {
        int64_t d1 = 0, d2 = 0;
        try_parse_i64(v1->str, &d1);
        try_parse_i64(v2->str, &d2);
        if (d1 < d2) return -1;
        if (d1 > d2) return 1;
        return 0;
    }
    /* String comparison */
    const char *s1 = v1->str;
    const char *s2 = v2->str;
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

/* --------------------------------------------------------------------------
 * AST & Grammar Definitions
 * -------------------------------------------------------------------------- */

typedef enum {
    EXPR_NUM,
    EXPR_STR,
    EXPR_VAR_NR,
    EXPR_VAR_NF,
    EXPR_VAR_FS,
    EXPR_VAR_OFS,
    EXPR_FIELD,
    EXPR_UNARY_MINUS,
    EXPR_NOT,
    EXPR_ADD,
    EXPR_SUB,
    EXPR_MUL,
    EXPR_DIV,
    EXPR_MOD,
    EXPR_EQ,
    EXPR_NE,
    EXPR_LT,
    EXPR_LE,
    EXPR_GT,
    EXPR_GE,
    EXPR_AND,
    EXPR_OR,
    EXPR_CONCAT
} expr_type_t;

typedef struct {
    expr_type_t type;
    int64_t num_val;
    char str_val[128];
    int left;
    int right;
} expr_node_t;

typedef enum {
    STMT_PRINT,
    STMT_PRINTF,
    STMT_ASSIGN_FIELD,
    STMT_EXPR
} stmt_type_t;

typedef struct {
    stmt_type_t type;
    int format_expr;
    int field_idx_expr;
    int val_expr;
    int args[AWK_MAX_ARGS];
    int num_args;
} awk_stmt_t;

typedef struct {
    bool has_pattern;
    bool pattern_is_regex;
    regex_t pattern_regex;
    int pattern_expr;
    awk_stmt_t stmts[AWK_MAX_STMTS];
    int num_stmts;
} awk_rule_t;

typedef struct {
    char str[AWK_FIELD_STR_CAP];
    size_t len;
} awk_field_t;

/* --------------------------------------------------------------------------
 * Static BSS Storage — zero dynamic allocation
 * -------------------------------------------------------------------------- */

static char s_line_buf[AWK_LINE_CAP];
static awk_field_t s_fields[AWK_MAX_FIELDS + 1]; /* 0=$0, 1..64=$1..$64 */
static int s_num_fields = 0;
static bool s_field_modified = false;
static uint64_t s_NR = 0;
static char s_fs_str[32] = " ";
static char s_fs_char = '\0'; /* '\0' means default whitespace FS */
static char s_ofs_str[32] = " ";

static expr_node_t s_nodes[AWK_MAX_NODES];
static int s_num_nodes = 0;
static awk_rule_t s_rules[AWK_MAX_RULES];
static int s_num_rules = 0;

typedef struct {
    int fd;
    unsigned char buf[TOOL_BUFFER_SIZE];
    size_t pos;
    size_t len;
    bool eof;
} line_reader_t;

static line_reader_t s_reader;
static char s_printf_buf[4096];

/* --------------------------------------------------------------------------
 * Lexer Tokens & Scanner
 * -------------------------------------------------------------------------- */

typedef enum {
    TOK_EOF,
    TOK_INT,
    TOK_FLOAT,
    TOK_STR,
    TOK_REGEX,
    TOK_IDENT,
    TOK_DOLLAR,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_ASSIGN,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_SEMI
} token_type_t;

typedef struct {
    token_type_t type;
    int64_t num_val;
    char text[256];
    size_t len;
} token_t;

static const char *s_src;
static token_t s_tok;
static bool s_can_be_div = false;

static void reconstruct_field_zero(void) {
    size_t pos = 0;
    size_t ofs_len = tool_length(s_ofs_str);
    for (int i = 1; i <= s_num_fields; i++) {
        if (i > 1) {
            for (size_t k = 0; k < ofs_len && pos + 1 < sizeof(s_fields[0].str); k++) {
                s_fields[0].str[pos++] = s_ofs_str[k];
            }
        }
        size_t flen = s_fields[i].len;
        for (size_t k = 0; k < flen && pos + 1 < sizeof(s_fields[0].str); k++) {
            s_fields[0].str[pos++] = s_fields[i].str[k];
        }
    }
    s_fields[0].str[pos] = '\0';
    s_fields[0].len = pos;
    s_field_modified = false;
}

static const char *get_field_str(int idx) {
    if (idx == 0) {
        if (s_field_modified) {
            reconstruct_field_zero();
        }
        return s_fields[0].str;
    }
    if (idx >= 1 && idx <= s_num_fields) {
        return s_fields[idx].str;
    }
    return "";
}

static int next_token(void) {
    const char *p = s_src;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        break;
    }

    if (!*p) {
        s_src = p;
        s_tok.type = TOK_EOF;
        s_tok.len = 0;
        s_tok.text[0] = '\0';
        s_can_be_div = false;
        return 0;
    }

    if (*p == ';' || *p == '\n') {
        p++;
        s_src = p;
        s_tok.type = TOK_SEMI;
        s_can_be_div = false;
        return 0;
    }
    if (*p == '{') {
        p++; s_src = p; s_tok.type = TOK_LBRACE; s_can_be_div = false; return 0;
    }
    if (*p == '}') {
        p++; s_src = p; s_tok.type = TOK_RBRACE; s_can_be_div = false; return 0;
    }
    if (*p == '(') {
        p++; s_src = p; s_tok.type = TOK_LPAREN; s_can_be_div = false; return 0;
    }
    if (*p == ')') {
        p++; s_src = p; s_tok.type = TOK_RPAREN; s_can_be_div = true; return 0;
    }
    if (*p == ',') {
        p++; s_src = p; s_tok.type = TOK_COMMA; s_can_be_div = false; return 0;
    }
    if (*p == '$') {
        p++; s_src = p; s_tok.type = TOK_DOLLAR; s_can_be_div = false; return 0;
    }
    if (*p == '[') {
        tool_error("awk", "arrays are unsupported", NULL);
        return 2;
    }
    if (*p == '?') {
        tool_error("awk", "ternary operator is unsupported", NULL);
        return 2;
    }

    /* Multi-character operators */
    if (*p == '=' && p[1] == '=') {
        p += 2; s_src = p; s_tok.type = TOK_EQ; s_can_be_div = false; return 0;
    }
    if (*p == '=') {
        p++; s_src = p; s_tok.type = TOK_ASSIGN; s_can_be_div = false; return 0;
    }
    if (*p == '!' && p[1] == '=') {
        p += 2; s_src = p; s_tok.type = TOK_NE; s_can_be_div = false; return 0;
    }
    if (*p == '!') {
        p++; s_src = p; s_tok.type = TOK_NOT; s_can_be_div = false; return 0;
    }
    if (*p == '<' && p[1] == '=') {
        p += 2; s_src = p; s_tok.type = TOK_LE; s_can_be_div = false; return 0;
    }
    if (*p == '<') {
        p++; s_src = p; s_tok.type = TOK_LT; s_can_be_div = false; return 0;
    }
    if (*p == '>' && p[1] == '=') {
        p += 2; s_src = p; s_tok.type = TOK_GE; s_can_be_div = false; return 0;
    }
    if (*p == '>') {
        p++; s_src = p; s_tok.type = TOK_GT; s_can_be_div = false; return 0;
    }
    if (*p == '&' && p[1] == '&') {
        p += 2; s_src = p; s_tok.type = TOK_AND; s_can_be_div = false; return 0;
    }
    if (*p == '|' && p[1] == '|') {
        p += 2; s_src = p; s_tok.type = TOK_OR; s_can_be_div = false; return 0;
    }
    if (*p == '+') {
        p++; s_src = p; s_tok.type = TOK_PLUS; s_can_be_div = false; return 0;
    }
    if (*p == '-') {
        p++; s_src = p; s_tok.type = TOK_MINUS; s_can_be_div = false; return 0;
    }
    if (*p == '*') {
        p++; s_src = p; s_tok.type = TOK_STAR; s_can_be_div = false; return 0;
    }
    if (*p == '%') {
        p++; s_src = p; s_tok.type = TOK_PERCENT; s_can_be_div = false; return 0;
    }

    /* Slash: division vs regex */
    if (*p == '/') {
        if (s_can_be_div) {
            p++; s_src = p; s_tok.type = TOK_SLASH; s_can_be_div = false; return 0;
        } else {
            p++;
            const char *start = p;
            while (*p && *p != '/') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p != '/') {
                tool_error("awk", "unterminated regex literal", NULL);
                return 2;
            }
            size_t len = (size_t)(p - start);
            if (len >= sizeof(s_tok.text)) len = sizeof(s_tok.text) - 1;
            for (size_t k = 0; k < len; k++) s_tok.text[k] = start[k];
            s_tok.text[len] = '\0';
            s_tok.len = len;
            s_tok.type = TOK_REGEX;
            p++;
            s_src = p;
            s_can_be_div = true;
            return 0;
        }
    }

    /* String literal */
    if (*p == '"') {
        p++;
        size_t len = 0;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                char esc = *p++;
                if (esc == 'n') esc = '\n';
                else if (esc == 't') esc = '\t';
                else if (esc == 'r') esc = '\r';
                if (len + 1 < sizeof(s_tok.text)) s_tok.text[len++] = esc;
            } else {
                if (len + 1 < sizeof(s_tok.text)) s_tok.text[len++] = *p++;
                else p++;
            }
        }
        if (*p != '"') {
            tool_error("awk", "unterminated string literal", NULL);
            return 2;
        }
        p++;
        s_tok.text[len] = '\0';
        s_tok.len = len;
        s_tok.type = TOK_STR;
        s_src = p;
        s_can_be_div = true;
        return 0;
    }

    /* Number literal */
    if ((*p >= '0' && *p <= '9') || (*p == '.' && (p[1] >= '0' && p[1] <= '9'))) {
        const char *start = p;
        bool is_float = false;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            is_float = true;
            p++;
            while (*p >= '0' && *p <= '9') p++;
        }
        size_t len = (size_t)(p - start);
        if (len >= sizeof(s_tok.text)) len = sizeof(s_tok.text) - 1;
        for (size_t k = 0; k < len; k++) s_tok.text[k] = start[k];
        s_tok.text[len] = '\0';
        s_tok.len = len;
        try_parse_i64(s_tok.text, &s_tok.num_val);
        s_tok.type = is_float ? TOK_FLOAT : TOK_INT;
        s_src = p;
        s_can_be_div = true;
        return 0;
    }

    /* Identifiers */
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        const char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len >= sizeof(s_tok.text)) len = sizeof(s_tok.text) - 1;
        for (size_t k = 0; k < len; k++) s_tok.text[k] = start[k];
        s_tok.text[len] = '\0';
        s_tok.len = len;
        s_src = p;

        /* Check unsupported control flow */
        if (tool_equal(s_tok.text, "if") || tool_equal(s_tok.text, "while") ||
            tool_equal(s_tok.text, "for") || tool_equal(s_tok.text, "do")) {
            char msg[48];
            char *m = msg;
            const char *pfx = "unsupported control flow '";
            while (*pfx) *m++ = *pfx++;
            for (size_t k = 0; k < len; k++) *m++ = s_tok.text[k];
            *m++ = '\'';
            *m++ = '\0';
            tool_error("awk", msg, NULL);
            return 2;
        }

        /* Check unsupported blocks */
        if (tool_equal(s_tok.text, "BEGIN") || tool_equal(s_tok.text, "END")) {
            char msg[48];
            char *m = msg;
            const char *pfx = "unsupported block '";
            while (*pfx) *m++ = *pfx++;
            for (size_t k = 0; k < len; k++) *m++ = s_tok.text[k];
            *m++ = '\'';
            *m++ = '\0';
            tool_error("awk", msg, NULL);
            return 2;
        }

        /* Check unsupported functions */
        if (tool_equal(s_tok.text, "length") || tool_equal(s_tok.text, "substr") ||
            tool_equal(s_tok.text, "split") || tool_equal(s_tok.text, "gsub") ||
            tool_equal(s_tok.text, "sub") || tool_equal(s_tok.text, "index") ||
            tool_equal(s_tok.text, "toupper") || tool_equal(s_tok.text, "tolower") ||
            tool_equal(s_tok.text, "sprintf") || tool_equal(s_tok.text, "system") ||
            tool_equal(s_tok.text, "getline")) {
            char msg[48];
            char *m = msg;
            const char *pfx = "unsupported function '";
            while (*pfx) *m++ = *pfx++;
            for (size_t k = 0; k < len; k++) *m++ = s_tok.text[k];
            *m++ = '\'';
            *m++ = '\0';
            tool_error("awk", msg, NULL);
            return 2;
        }

        s_tok.type = TOK_IDENT;
        s_can_be_div = true;
        return 0;
    }

    tool_error("awk", "unexpected character in program", NULL);
    return 2;
}

static bool can_start_expr(token_type_t t) {
    return (t == TOK_INT || t == TOK_FLOAT || t == TOK_STR || t == TOK_IDENT ||
            t == TOK_DOLLAR || t == TOK_LPAREN || t == TOK_NOT || t == TOK_MINUS || t == TOK_PLUS);
}

/* --------------------------------------------------------------------------
 * Recursive Descent Expression Parser
 * -------------------------------------------------------------------------- */

static int alloc_node(expr_type_t type) {
    if (s_num_nodes >= AWK_MAX_NODES) {
        tool_error("awk", "expression node limit exceeded", NULL);
        return -1;
    }
    int idx = s_num_nodes++;
    expr_node_t *n = &s_nodes[idx];
    n->type = type;
    n->num_val = 0.0;
    n->str_val[0] = '\0';
    n->left = -1;
    n->right = -1;
    return idx;
}

static int alloc_bin(expr_type_t type, int l, int r) {
    int idx = alloc_node(type);
    if (idx < 0) return -1;
    s_nodes[idx].left = l;
    s_nodes[idx].right = r;
    return idx;
}

static int parse_expr(int *out_node);
static int parse_unary(int *out_node);

static int parse_primary(int *out_node) {
    if (s_tok.type == TOK_INT || s_tok.type == TOK_FLOAT) {
        int idx = alloc_node(EXPR_NUM);
        if (idx < 0) return 2;
        s_nodes[idx].num_val = s_tok.num_val;
        format_i64(s_nodes[idx].str_val, sizeof(s_nodes[idx].str_val), s_tok.num_val);
        int r = next_token();
        if (r != 0) return r;
        *out_node = idx;
        return 0;
    }
    if (s_tok.type == TOK_STR) {
        int idx = alloc_node(EXPR_STR);
        if (idx < 0) return 2;
        size_t slen = s_tok.len;
        if (slen >= sizeof(s_nodes[idx].str_val)) slen = sizeof(s_nodes[idx].str_val) - 1;
        for (size_t k = 0; k < slen; k++) s_nodes[idx].str_val[k] = s_tok.text[k];
        s_nodes[idx].str_val[slen] = '\0';
        try_parse_i64(s_nodes[idx].str_val, &s_nodes[idx].num_val);
        int r = next_token();
        if (r != 0) return r;
        *out_node = idx;
        return 0;
    }
    if (s_tok.type == TOK_IDENT) {
        expr_type_t t = EXPR_STR;
        if (tool_equal(s_tok.text, "NR")) t = EXPR_VAR_NR;
        else if (tool_equal(s_tok.text, "NF")) t = EXPR_VAR_NF;
        else if (tool_equal(s_tok.text, "FS")) t = EXPR_VAR_FS;
        else if (tool_equal(s_tok.text, "OFS")) t = EXPR_VAR_OFS;
        else {
            /* General identifier treated as string literal / variable name */
            t = EXPR_STR;
        }
        int idx = alloc_node(t);
        if (idx < 0) return 2;
        size_t slen = s_tok.len;
        if (slen >= sizeof(s_nodes[idx].str_val)) slen = sizeof(s_nodes[idx].str_val) - 1;
        for (size_t k = 0; k < slen; k++) s_nodes[idx].str_val[k] = s_tok.text[k];
        s_nodes[idx].str_val[slen] = '\0';
        int r = next_token();
        if (r != 0) return r;
        *out_node = idx;
        return 0;
    }
    if (s_tok.type == TOK_LPAREN) {
        int r = next_token();
        if (r != 0) return r;
        r = parse_expr(out_node);
        if (r != 0) return r;
        if (s_tok.type != TOK_RPAREN) {
            tool_error("awk", "missing ')' in expression", NULL);
            return 2;
        }
        r = next_token();
        if (r != 0) return r;
        return 0;
    }

    tool_error("awk", "unexpected token in expression", NULL);
    return 2;
}

static int parse_unary(int *out_node) {
    if (s_tok.type == TOK_NOT) {
        int r = next_token();
        if (r != 0) return r;
        int child;
        r = parse_unary(&child);
        if (r != 0) return r;
        int idx = alloc_node(EXPR_NOT);
        if (idx < 0) return 2;
        s_nodes[idx].left = child;
        *out_node = idx;
        return 0;
    }
    if (s_tok.type == TOK_MINUS) {
        int r = next_token();
        if (r != 0) return r;
        int child;
        r = parse_unary(&child);
        if (r != 0) return r;
        int idx = alloc_node(EXPR_UNARY_MINUS);
        if (idx < 0) return 2;
        s_nodes[idx].left = child;
        *out_node = idx;
        return 0;
    }
    if (s_tok.type == TOK_PLUS) {
        int r = next_token();
        if (r != 0) return r;
        return parse_unary(out_node);
    }
    if (s_tok.type == TOK_DOLLAR) {
        int r = next_token();
        if (r != 0) return r;
        int child;
        r = parse_unary(&child);
        if (r != 0) return r;
        int idx = alloc_node(EXPR_FIELD);
        if (idx < 0) return 2;
        s_nodes[idx].left = child;
        *out_node = idx;
        return 0;
    }
    return parse_primary(out_node);
}

static int parse_multiplicative(int *out_node) {
    int left;
    int r = parse_unary(&left);
    if (r != 0) return r;

    while (s_tok.type == TOK_STAR || s_tok.type == TOK_SLASH || s_tok.type == TOK_PERCENT) {
        expr_type_t op = EXPR_MUL;
        if (s_tok.type == TOK_SLASH) op = EXPR_DIV;
        else if (s_tok.type == TOK_PERCENT) op = EXPR_MOD;
        r = next_token();
        if (r != 0) return r;
        int right;
        r = parse_unary(&right);
        if (r != 0) return r;
        left = alloc_bin(op, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_additive(int *out_node) {
    int left;
    int r = parse_multiplicative(&left);
    if (r != 0) return r;

    while (s_tok.type == TOK_PLUS || s_tok.type == TOK_MINUS) {
        expr_type_t op = (s_tok.type == TOK_PLUS) ? EXPR_ADD : EXPR_SUB;
        r = next_token();
        if (r != 0) return r;
        int right;
        r = parse_multiplicative(&right);
        if (r != 0) return r;
        left = alloc_bin(op, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_concat(int *out_node) {
    int left;
    int r = parse_additive(&left);
    if (r != 0) return r;

    while (can_start_expr(s_tok.type)) {
        int right;
        r = parse_additive(&right);
        if (r != 0) return r;
        left = alloc_bin(EXPR_CONCAT, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_comparison(int *out_node) {
    int left;
    int r = parse_concat(&left);
    if (r != 0) return r;

    if (s_tok.type == TOK_EQ || s_tok.type == TOK_NE ||
        s_tok.type == TOK_LT || s_tok.type == TOK_LE ||
        s_tok.type == TOK_GT || s_tok.type == TOK_GE) {
        expr_type_t op = EXPR_EQ;
        switch (s_tok.type) {
        case TOK_EQ: op = EXPR_EQ; break;
        case TOK_NE: op = EXPR_NE; break;
        case TOK_LT: op = EXPR_LT; break;
        case TOK_LE: op = EXPR_LE; break;
        case TOK_GT: op = EXPR_GT; break;
        case TOK_GE: op = EXPR_GE; break;
        default: break;
        }
        r = next_token();
        if (r != 0) return r;
        int right;
        r = parse_concat(&right);
        if (r != 0) return r;
        left = alloc_bin(op, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_logical_and(int *out_node) {
    int left;
    int r = parse_comparison(&left);
    if (r != 0) return r;

    while (s_tok.type == TOK_AND) {
        r = next_token();
        if (r != 0) return r;
        int right;
        r = parse_comparison(&right);
        if (r != 0) return r;
        left = alloc_bin(EXPR_AND, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_logical_or(int *out_node) {
    int left;
    int r = parse_logical_and(&left);
    if (r != 0) return r;

    while (s_tok.type == TOK_OR) {
        r = next_token();
        if (r != 0) return r;
        int right;
        r = parse_logical_and(&right);
        if (r != 0) return r;
        left = alloc_bin(EXPR_OR, left, right);
        if (left < 0) return 2;
    }
    *out_node = left;
    return 0;
}

static int parse_expr(int *out_node) {
    return parse_logical_or(out_node);
}

/* --------------------------------------------------------------------------
 * Statement & Program Parser
 * -------------------------------------------------------------------------- */

static int parse_stmt(awk_stmt_t *stmt) {
    stmt->num_args = 0;
    stmt->format_expr = -1;
    stmt->field_idx_expr = -1;
    stmt->val_expr = -1;

    if (s_tok.type == TOK_IDENT && tool_equal(s_tok.text, "print")) {
        stmt->type = STMT_PRINT;
        int r = next_token();
        if (r != 0) return r;
        if (s_tok.type != TOK_SEMI && s_tok.type != TOK_RBRACE && s_tok.type != TOK_EOF) {
            int arg;
            r = parse_expr(&arg);
            if (r != 0) return r;
            if (stmt->num_args < AWK_MAX_ARGS) stmt->args[stmt->num_args++] = arg;
            while (s_tok.type == TOK_COMMA) {
                r = next_token();
                if (r != 0) return r;
                r = parse_expr(&arg);
                if (r != 0) return r;
                if (stmt->num_args < AWK_MAX_ARGS) stmt->args[stmt->num_args++] = arg;
            }
        }
        return 0;
    }

    if (s_tok.type == TOK_IDENT && tool_equal(s_tok.text, "printf")) {
        stmt->type = STMT_PRINTF;
        int r = next_token();
        if (r != 0) return r;
        r = parse_expr(&stmt->format_expr);
        if (r != 0) return r;
        while (s_tok.type == TOK_COMMA) {
            r = next_token();
            if (r != 0) return r;
            int arg;
            r = parse_expr(&arg);
            if (r != 0) return r;
            if (stmt->num_args < AWK_MAX_ARGS) stmt->args[stmt->num_args++] = arg;
        }
        return 0;
    }

    if (s_tok.type == TOK_DOLLAR) {
        int r = next_token();
        if (r != 0) return r;
        int field_idx;
        r = parse_unary(&field_idx);
        if (r != 0) return r;
        if (s_tok.type == TOK_ASSIGN) {
            stmt->type = STMT_ASSIGN_FIELD;
            stmt->field_idx_expr = field_idx;
            r = next_token();
            if (r != 0) return r;
            r = parse_expr(&stmt->val_expr);
            if (r != 0) return r;
            return 0;
        } else {
            /* $field used in an expression statement */
            int full_node = alloc_node(EXPR_FIELD);
            if (full_node < 0) return 2;
            s_nodes[full_node].left = field_idx;
            stmt->type = STMT_EXPR;
            stmt->val_expr = full_node;
            return 0;
        }
    }

    stmt->type = STMT_EXPR;
    return parse_expr(&stmt->val_expr);
}

static int parse_program(const char *prog_text) {
    s_src = prog_text;
    s_num_nodes = 0;
    s_num_rules = 0;
    s_can_be_div = false;

    int r = next_token();
    if (r != 0) return r;

    while (s_tok.type != TOK_EOF) {
        if (s_tok.type == TOK_SEMI) {
            r = next_token();
            if (r != 0) return r;
            continue;
        }

        if (s_num_rules >= AWK_MAX_RULES) {
            tool_error("awk", "too many pattern-action rules", NULL);
            return 2;
        }

        awk_rule_t *rule = &s_rules[s_num_rules++];
        rule->has_pattern = false;
        rule->pattern_is_regex = false;
        rule->pattern_expr = -1;
        rule->num_stmts = 0;

        /* Check for Pattern */
        if (s_tok.type == TOK_LBRACE) {
            rule->has_pattern = false;
        } else if (s_tok.type == TOK_REGEX) {
            rule->has_pattern = true;
            rule->pattern_is_regex = true;
            if (!compile_pattern(&rule->pattern_regex, s_tok.text, s_tok.len, false)) {
                tool_error("awk", "invalid regular expression pattern", s_tok.text);
                return 2;
            }
            r = next_token();
            if (r != 0) return r;
        } else {
            rule->has_pattern = true;
            rule->pattern_is_regex = false;
            r = parse_expr(&rule->pattern_expr);
            if (r != 0) return r;
        }

        /* Check for Action Block { ... } */
        if (s_tok.type == TOK_LBRACE) {
            r = next_token();
            if (r != 0) return r;
            while (s_tok.type != TOK_RBRACE && s_tok.type != TOK_EOF) {
                if (s_tok.type == TOK_SEMI) {
                    r = next_token();
                    if (r != 0) return r;
                    continue;
                }
                if (rule->num_stmts >= AWK_MAX_STMTS) {
                    tool_error("awk", "too many statements in action block", NULL);
                    return 2;
                }
                awk_stmt_t *stmt = &rule->stmts[rule->num_stmts++];
                r = parse_stmt(stmt);
                if (r != 0) return r;
                if (s_tok.type == TOK_SEMI) {
                    r = next_token();
                    if (r != 0) return r;
                }
            }
            if (s_tok.type != TOK_RBRACE) {
                tool_error("awk", "missing '}' in action block", NULL);
                return 2;
            }
            r = next_token();
            if (r != 0) return r;
        } else {
            /* Pattern without action defaults to { print } */
            awk_stmt_t *stmt = &rule->stmts[rule->num_stmts++];
            stmt->type = STMT_PRINT;
            stmt->num_args = 0;
            stmt->format_expr = -1;
            stmt->field_idx_expr = -1;
            stmt->val_expr = -1;
        }
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Expression Evaluator
 * -------------------------------------------------------------------------- */

static awk_val_t eval_expr_node(int node_idx) {
    awk_val_t res;
    res.num = 0;
    res.str = "";
    res.len = 0;
    res.is_num = false;

    if (node_idx < 0 || node_idx >= s_num_nodes) return res;
    const expr_node_t *n = &s_nodes[node_idx];

    switch (n->type) {
    case EXPR_NUM:
        res.num = n->num_val;
        res.str = n->str_val;
        res.len = tool_length(n->str_val);
        res.is_num = true;
        return res;

    case EXPR_STR:
        res.num = n->num_val;
        res.str = n->str_val;
        res.len = tool_length(n->str_val);
        res.is_num = try_parse_i64(res.str, &res.num);
        return res;

    case EXPR_VAR_NR: {
        res.num = (int64_t)s_NR;
        res.is_num = true;
        char *buf = s_eval_buf[s_eval_buf_idx++ & 15];
        res.len = format_i64(buf, sizeof(s_eval_buf[0]), res.num);
        res.str = buf;
        return res;
    }

    case EXPR_VAR_NF: {
        res.num = (int64_t)s_num_fields;
        res.is_num = true;
        char *buf = s_eval_buf[s_eval_buf_idx++ & 15];
        res.len = format_i64(buf, sizeof(s_eval_buf[0]), res.num);
        res.str = buf;
        return res;
    }

    case EXPR_VAR_FS:
        res.num = 0;
        res.str = s_fs_str;
        res.len = tool_length(s_fs_str);
        res.is_num = false;
        return res;

    case EXPR_VAR_OFS:
        res.num = 0;
        res.str = s_ofs_str;
        res.len = tool_length(s_ofs_str);
        res.is_num = false;
        return res;

    case EXPR_FIELD: {
        awk_val_t child = eval_expr_node(n->left);
        int idx = (int)child.num;
        res.str = get_field_str(idx);
        res.len = (idx >= 0 && idx <= s_num_fields) ? s_fields[idx].len : tool_length(res.str);
        res.is_num = try_parse_i64(res.str, &res.num);
        return res;
    }

    case EXPR_UNARY_MINUS: {
        awk_val_t child = eval_expr_node(n->left);
        res.num = -child.num;
        res.is_num = true;
        char *buf = s_eval_buf[s_eval_buf_idx++ & 15];
        res.len = format_i64(buf, sizeof(s_eval_buf[0]), res.num);
        res.str = buf;
        return res;
    }

    case EXPR_NOT: {
        awk_val_t child = eval_expr_node(n->left);
        res.num = is_truthy(&child) ? 0 : 1;
        res.is_num = true;
        res.str = res.num ? "1" : "0";
        res.len = 1;
        return res;
    }

    case EXPR_ADD:
    case EXPR_SUB:
    case EXPR_MUL:
    case EXPR_DIV:
    case EXPR_MOD: {
        awk_val_t v1 = eval_expr_node(n->left);
        awk_val_t v2 = eval_expr_node(n->right);
        int64_t d = 0;
        if (n->type == EXPR_ADD) d = v1.num + v2.num;
        else if (n->type == EXPR_SUB) d = v1.num - v2.num;
        else if (n->type == EXPR_MUL) d = v1.num * v2.num;
        else if (n->type == EXPR_DIV) d = (v2.num != 0) ? (v1.num / v2.num) : 0;
        else if (n->type == EXPR_MOD) d = (v2.num != 0) ? (v1.num % v2.num) : 0;
        res.num = d;
        res.is_num = true;
        char *buf = s_eval_buf[s_eval_buf_idx++ & 15];
        res.len = format_i64(buf, sizeof(s_eval_buf[0]), d);
        res.str = buf;
        return res;
    }

    case EXPR_EQ:
    case EXPR_NE:
    case EXPR_LT:
    case EXPR_LE:
    case EXPR_GT:
    case EXPR_GE: {
        awk_val_t v1 = eval_expr_node(n->left);
        awk_val_t v2 = eval_expr_node(n->right);
        int cmp = compare_vals(&v1, &v2);
        bool match = false;
        switch (n->type) {
        case EXPR_EQ: match = (cmp == 0); break;
        case EXPR_NE: match = (cmp != 0); break;
        case EXPR_LT: match = (cmp < 0); break;
        case EXPR_LE: match = (cmp <= 0); break;
        case EXPR_GT: match = (cmp > 0); break;
        case EXPR_GE: match = (cmp >= 0); break;
        default: break;
        }
        res.num = match ? 1 : 0;
        res.is_num = true;
        res.str = match ? "1" : "0";
        res.len = 1;
        return res;
    }

    case EXPR_AND: {
        awk_val_t v1 = eval_expr_node(n->left);
        if (!is_truthy(&v1)) {
            res.num = 0;
            res.is_num = true;
            res.str = "0";
            res.len = 1;
            return res;
        }
        awk_val_t v2 = eval_expr_node(n->right);
        res.num = is_truthy(&v2) ? 1 : 0;
        res.is_num = true;
        res.str = res.num ? "1" : "0";
        res.len = 1;
        return res;
    }

    case EXPR_OR: {
        awk_val_t v1 = eval_expr_node(n->left);
        if (is_truthy(&v1)) {
            res.num = 1;
            res.is_num = true;
            res.str = "1";
            res.len = 1;
            return res;
        }
        awk_val_t v2 = eval_expr_node(n->right);
        res.num = is_truthy(&v2) ? 1 : 0;
        res.is_num = true;
        res.str = res.num ? "1" : "0";
        res.len = 1;
        return res;
    }

    case EXPR_CONCAT: {
        awk_val_t v1 = eval_expr_node(n->left);
        awk_val_t v2 = eval_expr_node(n->right);
        char *cbuf = s_concat_buf[s_concat_idx++ & 3];
        size_t l1 = v1.len;
        size_t l2 = v2.len;
        size_t pos = 0;
        for (size_t k = 0; k < l1 && pos + 1 < AWK_FIELD_STR_CAP; k++) cbuf[pos++] = v1.str[k];
        for (size_t k = 0; k < l2 && pos + 1 < AWK_FIELD_STR_CAP; k++) cbuf[pos++] = v2.str[k];
        cbuf[pos] = '\0';
        res.str = cbuf;
        res.len = pos;
        res.is_num = try_parse_i64(cbuf, &res.num);
        return res;
    }
    }

    return res;
}

/* --------------------------------------------------------------------------
 * Field Splitting & Execution Engine
 * -------------------------------------------------------------------------- */

static int split_line_into_fields(const char *line, size_t line_len) {
    s_num_fields = 0;
    s_field_modified = false;

    size_t cap0 = sizeof(s_fields[0].str) - 1;
    size_t copy_len = line_len < cap0 ? line_len : cap0;
    for (size_t k = 0; k < copy_len; k++) s_fields[0].str[k] = line[k];
    s_fields[0].str[copy_len] = '\0';
    s_fields[0].len = copy_len;

    const char *p = line;
    const char *end = line + line_len;

    if (s_fs_char == '\0') {
        /* Default whitespace FS: collapse spaces and tabs */
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        while (p < end) {
            if (s_num_fields >= AWK_MAX_FIELDS) {
                tool_error("awk", "line exceeds maximum fields (64)", NULL);
                return 2;
            }
            s_num_fields++;
            const char *f_start = p;
            while (p < end && *p != ' ' && *p != '\t') p++;
            size_t flen = (size_t)(p - f_start);
            if (flen >= sizeof(s_fields[s_num_fields].str)) {
                flen = sizeof(s_fields[s_num_fields].str) - 1;
            }
            for (size_t k = 0; k < flen; k++) s_fields[s_num_fields].str[k] = f_start[k];
            s_fields[s_num_fields].str[flen] = '\0';
            s_fields[s_num_fields].len = flen;

            while (p < end && (*p == ' ' || *p == '\t')) p++;
        }
    } else {
        /* Specific character FS */
        while (p < end) {
            if (s_num_fields >= AWK_MAX_FIELDS) {
                tool_error("awk", "line exceeds maximum fields (64)", NULL);
                return 2;
            }
            s_num_fields++;
            const char *f_start = p;
            while (p < end && *p != s_fs_char) p++;
            size_t flen = (size_t)(p - f_start);
            if (flen >= sizeof(s_fields[s_num_fields].str)) {
                flen = sizeof(s_fields[s_num_fields].str) - 1;
            }
            for (size_t k = 0; k < flen; k++) s_fields[s_num_fields].str[k] = f_start[k];
            s_fields[s_num_fields].str[flen] = '\0';
            s_fields[s_num_fields].len = flen;

            if (p < end && *p == s_fs_char) {
                p++;
                if (p == end) {
                    if (s_num_fields >= AWK_MAX_FIELDS) {
                        tool_error("awk", "line exceeds maximum fields (64)", NULL);
                        return 2;
                    }
                    s_num_fields++;
                    s_fields[s_num_fields].str[0] = '\0';
                    s_fields[s_num_fields].len = 0;
                }
            }
        }
    }

    return 0;
}

static int execute_printf(const awk_stmt_t *stmt) {
    awk_val_t fmt_val = eval_expr_node(stmt->format_expr);
    const char *fmt = fmt_val.str;
    size_t out_len = 0;
    int arg_idx = 0;

    const char *p = fmt;
    while (*p && out_len + 1 < sizeof(s_printf_buf)) {
        if (*p == '\\') {
            p++;
            char c = *p;
            if (!c) break;
            p++;
            if (c == 'n') s_printf_buf[out_len++] = '\n';
            else if (c == 't') s_printf_buf[out_len++] = '\t';
            else if (c == 'r') s_printf_buf[out_len++] = '\r';
            else if (c == '\\') s_printf_buf[out_len++] = '\\';
            else s_printf_buf[out_len++] = c;
        } else if (*p == '%') {
            p++;
            if (*p == '%') {
                p++;
                s_printf_buf[out_len++] = '%';
                continue;
            }
            bool left_align = false;
            bool zero_pad = false;
            while (*p == '-' || *p == '0') {
                if (*p == '-') left_align = true;
                else if (*p == '0') zero_pad = true;
                p++;
            }
            int width = 0;
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p++ - '0');
            }
            int prec = -1;
            if (*p == '.') {
                p++;
                prec = 0;
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
            char spec = *p ? *p++ : '\0';
            awk_val_t arg_val;
            if (arg_idx < stmt->num_args) {
                arg_val = eval_expr_node(stmt->args[arg_idx++]);
            } else {
                arg_val.num = 0;
                arg_val.str = "";
                arg_val.len = 0;
                arg_val.is_num = false;
            }

            if (spec == 's') {
                size_t slen = tool_length(arg_val.str);
                if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
                int pad = width > (int)slen ? width - (int)slen : 0;
                if (!left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = ' ';
                }
                for (size_t k = 0; k < slen && out_len + 1 < sizeof(s_printf_buf); k++) {
                    s_printf_buf[out_len++] = arg_val.str[k];
                }
                if (left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = ' ';
                }
            } else if (spec == 'd' || spec == 'i') {
                char nbuf[32];
                tool_format_i64(nbuf, (int64_t)arg_val.num);
                size_t nlen = tool_length(nbuf);
                int pad = width > (int)nlen ? width - (int)nlen : 0;
                char pad_ch = zero_pad ? '0' : ' ';
                if (!left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = pad_ch;
                }
                for (size_t k = 0; k < nlen && out_len + 1 < sizeof(s_printf_buf); k++) {
                    s_printf_buf[out_len++] = nbuf[k];
                }
                if (left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = ' ';
                }
            } else if (spec == 'u' || spec == 'x' || spec == 'X') {
                char nbuf[32];
                tool_format_u64(nbuf, (uint64_t)arg_val.num);
                size_t nlen = tool_length(nbuf);
                int pad = width > (int)nlen ? width - (int)nlen : 0;
                char pad_ch = zero_pad ? '0' : ' ';
                if (!left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = pad_ch;
                }
                for (size_t k = 0; k < nlen && out_len + 1 < sizeof(s_printf_buf); k++) {
                    s_printf_buf[out_len++] = nbuf[k];
                }
                if (left_align) {
                    while (pad-- > 0 && out_len + 1 < sizeof(s_printf_buf)) s_printf_buf[out_len++] = ' ';
                }
            } else if (spec == 'c') {
                char ch = arg_val.str[0] ? arg_val.str[0] : (char)arg_val.num;
                s_printf_buf[out_len++] = ch;
            } else if (spec == 'f' || spec == 'g') {
                char nbuf[64];
                format_i64(nbuf, sizeof(nbuf), arg_val.num);
                size_t nlen = tool_length(nbuf);
                for (size_t k = 0; k < nlen && out_len + 1 < sizeof(s_printf_buf); k++) {
                    s_printf_buf[out_len++] = nbuf[k];
                }
            } else {
                s_printf_buf[out_len++] = spec;
            }
        } else {
            s_printf_buf[out_len++] = *p++;
        }
    }

    if (out_len > 0) {
        return tool_write("awk", s_printf_buf, out_len);
    }
    return 0;
}

static int execute_statement(const awk_stmt_t *stmt) {
    switch (stmt->type) {
    case STMT_PRINT: {
        if (stmt->num_args == 0) {
            const char *f0 = get_field_str(0);
            size_t len = tool_length(f0);
            if (len > 0) {
                int r = tool_write("awk", f0, len);
                if (r != 0) return r;
            }
            return tool_write("awk", "\n", 1);
        } else {
            size_t ofs_len = tool_length(s_ofs_str);
            for (int i = 0; i < stmt->num_args; i++) {
                if (i > 0 && ofs_len > 0) {
                    int r = tool_write("awk", s_ofs_str, ofs_len);
                    if (r != 0) return r;
                }
                awk_val_t val = eval_expr_node(stmt->args[i]);
                size_t vlen = tool_length(val.str);
                if (vlen > 0) {
                    int r = tool_write("awk", val.str, vlen);
                    if (r != 0) return r;
                }
            }
            return tool_write("awk", "\n", 1);
        }
    }

    case STMT_PRINTF:
        return execute_printf(stmt);

    case STMT_ASSIGN_FIELD: {
        awk_val_t idx_val = eval_expr_node(stmt->field_idx_expr);
        int idx = (int)idx_val.num;
        if (idx < 1 || idx > AWK_MAX_FIELDS) {
            tool_error("awk", "invalid field assignment index", NULL);
            return 2;
        }
        awk_val_t new_val = eval_expr_node(stmt->val_expr);
        if (idx > s_num_fields) {
            for (int k = s_num_fields + 1; k <= idx; k++) {
                s_fields[k].str[0] = '\0';
                s_fields[k].len = 0;
            }
            s_num_fields = idx;
        }
        size_t slen = tool_length(new_val.str);
        if (slen >= sizeof(s_fields[idx].str)) slen = sizeof(s_fields[idx].str) - 1;
        for (size_t k = 0; k < slen; k++) s_fields[idx].str[k] = new_val.str[k];
        s_fields[idx].str[slen] = '\0';
        s_fields[idx].len = slen;
        s_field_modified = true;
        return 0;
    }

    case STMT_EXPR:
        (void)eval_expr_node(stmt->val_expr);
        return 0;
    }

    return 0;
}

static int process_line(const char *line, size_t line_len) {
    s_NR++;
    int r = split_line_into_fields(line, line_len);
    if (r != 0) return r;

    for (int i = 0; i < s_num_rules; i++) {
        awk_rule_t *rule = &s_rules[i];
        bool matched = false;

        if (!rule->has_pattern) {
            matched = true;
        } else if (rule->pattern_is_regex) {
            const char *f0 = get_field_str(0);
            matched = regex_matches(&rule->pattern_regex, f0, tool_length(f0));
        } else {
            awk_val_t val = eval_expr_node(rule->pattern_expr);
            matched = is_truthy(&val);
        }

        if (matched) {
            for (int s = 0; s < rule->num_stmts; s++) {
                r = execute_statement(&rule->stmts[s]);
                if (r != 0) return r;
            }
        }
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Line Reader & File Processing
 * -------------------------------------------------------------------------- */

static int reader_getc(line_reader_t *r) {
    if (r->pos >= r->len) {
        if (r->eof) return -1;
        long n = tool_syscall(SYS_READ, (uintptr_t)r->fd, (uintptr_t)r->buf, sizeof(r->buf));
        if (n <= 0) {
            r->eof = true;
            return -1;
        }
        r->pos = 0;
        r->len = (size_t)n;
    }
    return r->buf[r->pos++];
}

static bool reader_read_line(line_reader_t *r, char *line_buf, size_t cap, size_t *out_len) {
    size_t len = 0;
    int c;
    while ((c = reader_getc(r)) >= 0) {
        if (c == '\n') {
            if (len > 0 && line_buf[len - 1] == '\r') {
                len--;
            }
            line_buf[len] = '\0';
            *out_len = len;
            return true;
        }
        if (len + 1 < cap) {
            line_buf[len++] = (char)c;
        } else {
            tool_error("awk", "line too long", NULL);
            return false;
        }
    }
    if (len > 0) {
        if (line_buf[len - 1] == '\r') {
            len--;
        }
        line_buf[len] = '\0';
        *out_len = len;
        return true;
    }
    return false;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: awk [OPTIONS] 'SCRIPT' [FILE ...]\n"
        "Pattern scanning and processing language (bounded stream subset).\n\n"
        "Options:\n"
        "  -F SEP        Set field separator (default: whitespace)\n"
        "  --help        Display this help text and exit\n\n"
        "Supported Program Structure:\n"
        "  pattern { action }    Execute action when pattern matches\n"
        "  { action }            Execute action on every line\n"
        "  pattern               Print line when pattern matches\n\n"
        "Patterns:\n"
        "  /regex/               Regular expression match\n"
        "  expr                  Comparison, relational, or logical expression\n\n"
        "Built-in Variables:\n"
        "  NR                    Record (line) number (1-based)\n"
        "  NF                    Number of fields on current line\n"
        "  FS                    Field separator\n"
        "  OFS                   Output field separator (default: space)\n\n"
        "Fields:\n"
        "  $0                    The entire line\n"
        "  $1, $2, ... $NF       Fields by index or expression\n\n"
        "Actions & Statements:\n"
        "  print                 Print $0 followed by newline\n"
        "  print expr, expr      Print expressions separated by OFS\n"
        "  print expr expr       Print concatenated expressions\n"
        "  printf format, args   Formatted print\n"
        "  $N = expr             Assign to field\n";
    return tool_write("awk", help_text, sizeof(help_text) - 1);
}

/* --------------------------------------------------------------------------
 * Entry Point
 * -------------------------------------------------------------------------- */

static char s_script_buf[8192];

int awk_main(int argc, char **argv) {
    s_NR = 0;
    s_num_fields = 0;
    s_field_modified = false;
    s_fs_str[0] = ' '; s_fs_str[1] = '\0';
    s_fs_char = '\0';
    s_ofs_str[0] = ' '; s_ofs_str[1] = '\0';

    const char *prog_arg = NULL;
    bool script_from_file = false;
    const char *files[AWK_MAX_FILES];
    int num_files = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            return print_help();
        }
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] == '-' && arg[1]) {
            if (arg[1] == 'F') {
                const char *sep = NULL;
                if (arg[2] != '\0') {
                    sep = arg + 2;
                } else if (++i < argc) {
                    sep = argv[i];
                } else {
                    tool_error("awk", "option requires an argument -- 'F'", NULL);
                    return 2;
                }
                if (sep[0] == '\\' && sep[1] == 't') {
                    s_fs_char = '\t';
                    s_fs_str[0] = '\t'; s_fs_str[1] = '\0';
                } else {
                    s_fs_char = sep[0];
                    size_t slen = tool_length(sep);
                    if (slen >= sizeof(s_fs_str)) slen = sizeof(s_fs_str) - 1;
                    for (size_t k = 0; k < slen; k++) s_fs_str[k] = sep[k];
                    s_fs_str[slen] = '\0';
                }
            } else if (arg[1] == 'f') {
                if (script_from_file) {
                    tool_error("awk", "multiple -f scripts are unsupported", NULL);
                    return 2;
                }
                const char *fpath = NULL;
                if (arg[2] != '\0') {
                    fpath = arg + 2;
                } else if (++i < argc) {
                    fpath = argv[i];
                } else {
                    tool_error("awk", "option requires an argument -- 'f'", NULL);
                    return 2;
                }
                long fd = tool_syscall(SYS_OPEN, (uintptr_t)fpath, VFS_O_RDONLY, 0);
                if (fd < 0) {
                    tool_error("awk", "cannot open script file", fpath);
                    return 2;
                }
                long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)s_script_buf, sizeof(s_script_buf) - 1);
                tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                if (n < 0) {
                    tool_error("awk", "cannot read script file", fpath);
                    return 2;
                }
                s_script_buf[n] = '\0';
                prog_arg = s_script_buf;
                script_from_file = true;
            } else if (arg[1] == 'v') {
                tool_error("awk", "option '-v' is unsupported", NULL);
                return 2;
            } else {
                tool_error("awk", "invalid option; use --help", NULL);
                return 2;
            }
        } else {
            break;
        }
    }

    if (!script_from_file) {
        if (i >= argc) {
            tool_error("awk", "missing program; use --help", NULL);
            return 2;
        }
        prog_arg = argv[i++];
    }

    for (; i < argc; i++) {
        if (num_files < AWK_MAX_FILES) {
            files[num_files++] = argv[i];
        }
    }

    int r = parse_program(prog_arg);
    if (r != 0) return r;

    if (num_files == 0) {
        for (size_t k = 0; k < sizeof(s_reader); k++) ((char *)&s_reader)[k] = 0;
        s_reader.fd = 0;
        size_t line_len = 0;
        while (reader_read_line(&s_reader, s_line_buf, sizeof(s_line_buf), &line_len)) {
            r = process_line(s_line_buf, line_len);
            if (r != 0) return r;
        }
        return 0;
    }

    for (int f = 0; f < num_files; f++) {
        const char *fpath = files[f];
        int fd = 0;
        if (!tool_equal(fpath, "-")) {
            long open_r = tool_syscall(SYS_OPEN, (uintptr_t)fpath, VFS_O_RDONLY, 0);
            if (open_r < 0) {
                tool_error("awk", "cannot open", fpath);
                return 2;
            }
            fd = (int)open_r;
        }

        for (size_t k = 0; k < sizeof(s_reader); k++) ((char *)&s_reader)[k] = 0;
        s_reader.fd = fd;
        size_t line_len = 0;
        while (reader_read_line(&s_reader, s_line_buf, sizeof(s_line_buf), &line_len)) {
            r = process_line(s_line_buf, line_len);
            if (r != 0) {
                if (fd != 0) tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                return r;
            }
        }

        if (fd != 0) {
            tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        }
    }

    return 0;
}
