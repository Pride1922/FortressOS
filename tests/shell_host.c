#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lineedit.h"
#include "lexer.h"
#include "parser.h"
#include "vars.h"
#include "alias.h"
#include "expand.h"
#include "redir.h"

static int s_mock_fds[32];
static int s_mock_flags[32];
static int s_mock_next_inode = 100;

static void mock_reset_fds(void) {
    for (int i = 0; i < 32; i++) {
        s_mock_fds[i] = -1;
        s_mock_flags[i] = 0;
    }
    /* Standard fds open by default */
    s_mock_fds[0] = 1;
    s_mock_fds[1] = 2;
    s_mock_fds[2] = 3;
}

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_OPEN) {
        const char *p = (const char *)a;
        if (!strcmp(p, "/nonexistent")) return SYSCALL_ENOENT;
        for (int i = 0; i < 32; i++) {
            if (s_mock_fds[i] == -1) {
                s_mock_fds[i] = ++s_mock_next_inode;
                s_mock_flags[i] = ((int)b & VFS_O_CLOEXEC) ? FD_CLOEXEC : 0;
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (fd < 0 || fd >= 32 || s_mock_fds[fd] == -1) return SYSCALL_EBADF;
        s_mock_fds[fd] = -1;
        s_mock_flags[fd] = 0;
        return 0;
    }
    if (nr == SYS_DUP2) {
        int oldfd = (int)a;
        int newfd = (int)b;
        if (oldfd < 0 || oldfd >= 32 || s_mock_fds[oldfd] == -1) return SYSCALL_EBADF;
        if (newfd < 0 || newfd >= 32) return SYSCALL_EBADF;
        s_mock_fds[newfd] = s_mock_fds[oldfd];
        s_mock_flags[newfd] = 0;
        return newfd;
    }
    if (nr == SYS_FCNTL) {
        int fd = (int)a;
        int cmd = (int)b;
        if (fd < 0 || fd >= 32 || s_mock_fds[fd] == -1) return SYSCALL_EBADF;
        if (cmd == F_GETFD) return s_mock_flags[fd];
        if (cmd == F_SETFD) {
            s_mock_flags[fd] = (int)c;
            return 0;
        }
        if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
            int min_fd = (int)c;
            if (min_fd < 0 || min_fd >= 32) return SYSCALL_EINVAL;
            for (int i = min_fd; i < 32; i++) {
                if (s_mock_fds[i] == -1) {
                    s_mock_fds[i] = s_mock_fds[fd];
                    s_mock_flags[i] = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                    return i;
                }
            }
            return SYSCALL_EMFILE;
        }
        return SYSCALL_EINVAL;
    }
    return -1;
}

long puts_err(const char *s) {
    return (long)strlen(s);
}

void file_error_err(long error) {
    (void)error;
}

static line_editor_t e;
static void feed(const char *s) { while(*s) lineedit_byte(&e,(unsigned char)*s++); }

static unsigned pipeline_diagnostics;
static long pipeline_diagnostic(const char *message) {
    assert(!strcmp(message, "pipeline: not yet supported\n"));
    pipeline_diagnostics++;
    return (long)strlen(message);
}

static void test_pipelines(void) {
    static parse_tree_t tree;
    static lexer_t lex;
    token_t tok;
    lexer_init(&lex, "a|b||c");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "a"));
    assert(lexer_next(&lex, &tok) == TOK_PIPE && tok.len == 1);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "b"));
    assert(lexer_next(&lex, &tok) == TOK_OR && tok.len == 2);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "c"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    static const struct {
        const char *src;
        int count;
        enum cmd_op ops[4];
    } cases[] = {
        {"a | b", 2, {CMD_OP_PIPE, CMD_OP_NONE}},
        {"a|b|c", 3, {CMD_OP_PIPE, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a | b && c", 3, {CMD_OP_PIPE, CMD_OP_AND, CMD_OP_NONE}},
        {"a && b | c", 3, {CMD_OP_AND, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a ; b | c", 3, {CMD_OP_SEMI, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a | b ; c | d", 4, {CMD_OP_PIPE, CMD_OP_SEMI, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a | b || c | d", 4, {CMD_OP_PIPE, CMD_OP_OR, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a || b | c", 3, {CMD_OP_OR, CMD_OP_PIPE, CMD_OP_NONE}},
        {"a | b && c | d", 4, {CMD_OP_PIPE, CMD_OP_AND, CMD_OP_PIPE, CMD_OP_NONE}},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        assert(parser_parse(cases[k].src, &tree) == PARSE_OK);
        assert(tree.cmd_count == cases[k].count);
        for (int i = 0; i < tree.cmd_count; i++) {
            assert(tree.cmds[i].argc == 1 && tree.cmds[i].argv[0][0] == 'a' + i);
            assert(tree.cmds[i].argv[0][1] == '\0');
            assert(tree.cmds[i].next_op == cases[k].ops[i]);
        }
    }
    assert(parser_parse("a|b|c|d|e|f|g|h", &tree) == PARSE_OK && tree.cmd_count == 8);
    assert(parser_parse("a|b|c|d|e|f|g|h|i", &tree) == PARSE_SYNTAX_ERROR);
    assert(!strcmp(tree.error_msg, "pipeline too long (maximum 8 stages)"));
    /* Each chain operator resets the stage limit, without changing MAX_CMDS. */
    static const char *chains[] = {
        "a|b|c|d|e|f|g|h;a|b|c|d|e|f|g|h",
        "a|b|c|d|e|f|g|h&&a|b|c|d|e|f|g|h",
        "a|b|c|d|e|f|g|h||a|b|c|d|e|f|g|h",
    };
    for (size_t k = 0; k < sizeof(chains) / sizeof(chains[0]); k++)
        assert(parser_parse(chains[k], &tree) == PARSE_OK && tree.cmd_count == 16);
    static const char *bad[] = {"| a", "a |", "a | | b", "a | ; b", "a | && b",
                               "a | || b", "a ||| b", "a |\nb", "a | # empty"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        assert(parser_parse(bad[k], &tree) == PARSE_SYNTAX_ERROR);
    static const char *quoted[] = {"echo \"a|b\"", "echo 'a|b'", "echo a\\|b"};
    static const uint8_t flags[] = {QUOTE_DOUBLE, QUOTE_SINGLE, QUOTE_ESCAPED};
    for (size_t k = 0; k < sizeof(quoted) / sizeof(quoted[0]); k++) {
        assert(parser_parse(quoted[k], &tree) == PARSE_OK && tree.cmd_count == 1);
        assert(!strcmp(tree.cmds[0].argv[1], "a|b"));
        assert(tree.cmds[0].quote_flags[1][1] == flags[k]);
        assert(tree.cmds[0].next_op == CMD_OP_NONE);
        assert(parser_execution_guard(&tree, pipeline_diagnostic) == 0);
    }
    assert(pipeline_diagnostics == 0);
    assert(parser_parse("a > file | b", &tree) == PARSE_OK);
    assert(tree.cmds[0].redir_count == 1 && tree.cmds[0].redirs[0].redir_op == REDIR_OUT);
    assert(!strcmp(tree.cmds[0].redirs[0].target, "file"));
    assert(tree.cmds[0].next_op == CMD_OP_PIPE && tree.cmds[1].next_op == CMD_OP_NONE);
    assert(parser_parse("! a | b", &tree) == PARSE_OK && tree.cmds[0].negate);
    mock_reset_fds();
    int inode_before = s_mock_next_inode;
    assert(parser_parse("echo before; > /mnt/canary | /bin/hello", &tree) == PARSE_OK);
    assert(parser_execution_guard(&tree, pipeline_diagnostic) == 1);
    assert(pipeline_diagnostics == 1 && s_mock_next_inode == inode_before);
    for (int i = 3; i < 32; i++) assert(s_mock_fds[i] == -1);
    /* Reusing the same tree must not retain a pipeline marker or diagnostic. */
    assert(parser_parse("echo recovered", &tree) == PARSE_OK);
    assert(parser_execution_guard(&tree, pipeline_diagnostic) == 0);
    assert(pipeline_diagnostics == 1);
    puts("PASS shell pipelines: flat operators, stage limits, quotes, redirections and interim guard");
}

int main(void) {
    lineedit_init(&e);
    feed("echo ac\033[Db\033[F"); assert(!strcmp(e.text,"echo abc"));
    feed("\033[H\033[3~"); assert(!strcmp(e.text,"cho abc"));
    feed("\001e\005\027"); assert(!strcmp(e.text,"echo "));
    feed("\031"); assert(!strcmp(e.text,"echo abc"));
    feed("\001\013"); assert(!e.len); feed("\031"); assert(!strcmp(e.text,"echo abc"));
    assert(lineedit_byte(&e,'\n')==EDIT_ACCEPT);
    lineedit_init(&e); feed("\033[200~unfinished");
    assert(lineedit_byte(&e,3)==EDIT_CANCEL);
    history_clear(); history_add("echo first"); history_add("echo second"); history_add("echo second");
    assert(history_count()==2);
    lineedit_init(&e); feed("draft\033[A"); assert(!strcmp(e.text,"echo second"));
    feed("\033[A"); assert(!strcmp(e.text,"echo first"));
    feed("\033[B\033[B"); assert(!strcmp(e.text,"draft"));
    feed("\022first"); assert(e.search && !strcmp(e.text,"echo first"));
    assert(lineedit_byte(&e,'\n')==EDIT_CHANGED); assert(!e.search);
    assert(lineedit_byte(&e,'\n')==EDIT_ACCEPT);
    lineedit_init(&e); feed("draft\022first\033"); lineedit_timeout(&e); assert(!strcmp(e.text,"draft"));
    lineedit_init(&e); feed("\033[200~echo a\necho b\033[201~");
    assert(!strcmp(e.text,"echo a echo b") && e.review);
    assert(lineedit_byte(&e,'\n')==EDIT_CHANGED);
    assert(lineedit_byte(&e,'\n')==EDIT_ACCEPT);
    lineedit_init(&e);
    for(unsigned i=0;i<LINE_CAP+20;i++) lineedit_byte(&e,'x');
    assert(e.len==LINE_CAP-1 && e.blocked);
    assert(lineedit_byte(&e,'\n')!=EDIT_ACCEPT);
    lineedit_lost(&e); assert(lineedit_byte(&e,3)==EDIT_CANCEL);
    lineedit_init(&e); assert(lineedit_byte(&e,4)==EDIT_EOF);
    for(unsigned i=0;i<1100;i++) { char s[40]; snprintf(s,sizeof(s),"echo %u",i); history_add(s); }
    assert(history_count()==1000 && !strcmp(history_get(999),"echo 1099"));
    static char big[LINE_CAP]; memset(big,'a',LINE_CAP-1); big[LINE_CAP-1]=0;
    for(unsigned i=0;i<100;i++) { big[0]=(char)('a'+i%26); history_add(big); }
    assert(history_count()<1000);

    /* Test Tab completion key in lineedit */
    lineedit_init(&e);
    feed("ec");
    assert(lineedit_byte(&e, '\t') == EDIT_COMPLETE);

    /* Deterministic hostile byte stream: every operation preserves bounds/NUL. */
    lineedit_init(&e); unsigned seed=7;
    for(unsigned i=0;i<100000;i++) {
        seed=seed*1664525+1013904223;
        enum edit_result r=lineedit_byte(&e,(unsigned char)(seed>>24));
        assert(e.cursor<=e.len && e.len<LINE_CAP && e.text[e.len]==0);
        if(r==EDIT_CANCEL || r==EDIT_EOF || r==EDIT_ACCEPT) lineedit_init(&e);
    }
    puts("PASS shell editor: editing, history budgets/search, paste, overflow, hostile bytes");

    /* Test S3 Lexer */
    lexer_t lex;
    token_t tok;

    /* 1. Single quotes preserve literal chars, including operators */
    lexer_init(&lex, "echo 'a && b' 'c || d'");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "a && b"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "c || d"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 2. Double quotes preserve spaces and handle escapes */
    lexer_init(&lex, "echo \"hello \\\"world\\\"\" \"line\\nbreak\"");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "hello \"world\""));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "line\nbreak"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 3. Concatenation of quoted and unquoted parts */
    lexer_init(&lex, "foo\"bar\"'baz'");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "foobarbaz"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 4. Empty arguments are preserved */
    lexer_init(&lex, "echo \"\" ''");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "") && tok.len == 0);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "") && tok.len == 0);
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 5. Unquoted backslash escapes */
    lexer_init(&lex, "echo hello\\ world a\\\\b");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "hello world"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "a\\b"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 6. Comments at token start */
    lexer_init(&lex, "echo hello # this is a comment\nls");
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "hello"));
    assert(lexer_next(&lex, &tok) == TOK_SEMI);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "ls"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 7. Operators and pipeline negation */
    lexer_init(&lex, "! true && false || echo ok ; pwd");
    assert(lexer_next(&lex, &tok) == TOK_BANG);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "true"));
    assert(lexer_next(&lex, &tok) == TOK_AND);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "false"));
    assert(lexer_next(&lex, &tok) == TOK_OR);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "echo"));
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "ok"));
    assert(lexer_next(&lex, &tok) == TOK_SEMI);
    assert(lexer_next(&lex, &tok) == TOK_WORD && !strcmp(tok.value, "pwd"));
    assert(lexer_next(&lex, &tok) == TOK_EOF);

    /* 8. Incomplete checks */
    assert(lexer_check_incomplete("echo 'open") == LEX_INCOMPLETE_SQUOTE);
    assert(lexer_check_incomplete("echo \"open") == LEX_INCOMPLETE_DQUOTE);
    assert(lexer_check_incomplete("echo foo \\") == LEX_INCOMPLETE_BACKSLASH);
    assert(lexer_check_incomplete("echo foo &&") == LEX_INCOMPLETE_OP);
    assert(lexer_check_incomplete("echo foo ||") == LEX_INCOMPLETE_OP);
    assert(lexer_check_incomplete("echo 'closed'") == LEX_OK);
    assert(lexer_check_incomplete("echo \"closed\"") == LEX_OK);

    puts("PASS shell lexer: quotes, escapes, concatenation, empty words, comments, incomplete states");

    /* Test S3 Parser */
    parse_tree_t tree;

    /* 1. Parse simple command with quotes */
    assert(parser_parse("echo 'a && b'", &tree) == PARSE_OK);
    assert(tree.cmd_count == 1);
    assert(tree.cmds[0].argc == 2);
    assert(!strcmp(tree.cmds[0].argv[0], "echo"));
    assert(!strcmp(tree.cmds[0].argv[1], "a && b"));
    assert(!tree.cmds[0].negate);
    assert(tree.cmds[0].next_op == CMD_OP_NONE);

    /* 2. Parse command chain with && and || */
    assert(parser_parse("false && echo no || echo yes", &tree) == PARSE_OK);
    assert(tree.cmd_count == 3);
    assert(tree.cmds[0].argc == 1 && !strcmp(tree.cmds[0].argv[0], "false") && tree.cmds[0].next_op == CMD_OP_AND);
    assert(tree.cmds[1].argc == 2 && !strcmp(tree.cmds[1].argv[0], "echo") && tree.cmds[1].next_op == CMD_OP_OR);
    assert(tree.cmds[2].argc == 2 && !strcmp(tree.cmds[2].argv[0], "echo") && tree.cmds[2].next_op == CMD_OP_NONE);

    /* 3. Pipeline negation '!' */
    assert(parser_parse("! true", &tree) == PARSE_OK);
    assert(tree.cmd_count == 1);
    assert(tree.cmds[0].negate == true);
    assert(!strcmp(tree.cmds[0].argv[0], "true"));

    /* 4. Sequential list ';' */
    assert(parser_parse("echo a ; echo b ; echo c", &tree) == PARSE_OK);
    assert(tree.cmd_count == 3);
    assert(tree.cmds[0].next_op == CMD_OP_SEMI);
    assert(tree.cmds[1].next_op == CMD_OP_SEMI);
    assert(tree.cmds[2].next_op == CMD_OP_NONE);

    /* 5. Syntax error handling */
    assert(parser_parse("&& foo", &tree) == PARSE_SYNTAX_ERROR);
    const char *bad_redirs[] = {
        "echo ok 2>&257", "echo ok 99>out", "echo ok 999999999999999>out",
        "echo ok 2>&999999999999999999999999", "echo ok 2>& 999999999999999999999999",
        "echo ok 32>out", "echo ok 2<&32"
    };
    for (size_t k = 0; k < sizeof(bad_redirs) / sizeof(bad_redirs[0]); k++)
        assert(parser_parse(bad_redirs[k], &tree) == PARSE_SYNTAX_ERROR);
    assert(parser_parse("echo ok 31>&30 2<& 0 1>&-", &tree) == PARSE_OK);
    assert(tree.cmds[0].redir_count == 3);
    assert(tree.cmds[0].redirs[0].redir_fd == 31 && tree.cmds[0].redirs[0].redir_dup_fd == 30);
    assert(tree.cmds[0].redirs[1].redir_dup_fd == 0);
    assert(tree.cmds[0].redirs[2].redir_op == REDIR_CLOSE);
    assert(parser_parse("|| foo", &tree) == PARSE_SYNTAX_ERROR);
    assert(parser_parse("!", &tree) == PARSE_SYNTAX_ERROR);

    /* 6. Incomplete input returns PARSE_INCOMPLETE */
    assert(parser_parse("echo 'unterminated", &tree) == PARSE_INCOMPLETE);
    assert(parser_parse("echo foo &&", &tree) == PARSE_INCOMPLETE);

    puts("PASS shell parser: AST building, precedence, operators, negation, syntax errors");

    /* Test S5 Variables */
    vars_init();
    assert(!strcmp(vars_get("PATH"), "/bin"));
    assert(!strcmp(vars_get("HOME"), "/"));
    assert(!strcmp(vars_get("PS1"), "fortress> "));
    assert(vars_get("NONEXISTENT") == NULL);

    assert(vars_set("TEST_VAR", "12345", false) == 0);
    assert(!strcmp(vars_get("TEST_VAR"), "12345"));

    assert(vars_export("EXPORTED_VAR=hello") == 0);
    assert(!strcmp(vars_get("EXPORTED_VAR"), "hello"));

    assert(vars_unset("TEST_VAR") == 0);
    assert(vars_get("TEST_VAR") == NULL);

    assert(vars_is_valid_name("valid_name_1"));
    assert(vars_is_valid_name("_valid"));
    assert(!vars_is_valid_name("1invalid"));
    assert(!vars_is_valid_name("invalid-hyphen"));

    char aname[MAX_VAR_NAME];
    const char *aval = NULL;
    assert(vars_is_assignment("FOO=bar", aname, sizeof(aname), &aval) && !strcmp(aname, "FOO") && !strcmp(aval, "bar"));
    assert(!vars_is_assignment("=bar", aname, sizeof(aname), &aval));
    assert(!vars_is_assignment("123=bar", aname, sizeof(aname), &aval));
    assert(!vars_is_assignment("no_equals", aname, sizeof(aname), &aval));

    /* Command-local variable scoping */
    local_var_scope_t scope;
    vars_set("SCOPED", "parent_val", false);
    vars_scope_begin(&scope);
    vars_scope_set(&scope, "SCOPED", "child_val");
    vars_scope_set(&scope, "TEMP_NEW", "temp_val");
    assert(!strcmp(vars_get("SCOPED"), "child_val"));
    assert(!strcmp(vars_get("TEMP_NEW"), "temp_val"));
    vars_scope_end(&scope);
    assert(!strcmp(vars_get("SCOPED"), "parent_val"));
    assert(vars_get("TEMP_NEW") == NULL);

    /* Envp vector construction */
    static char env_strs[32][MAX_VAR_NAME + MAX_VAR_VAL + 2];
    static const char *env_ptrs[33];
    int env_count = vars_build_envp(env_strs, env_ptrs);
    assert(env_count >= 2); /* PATH and HOME */
    assert(env_ptrs[env_count] == NULL);

    puts("PASS shell variables: defaults, assignments, export, unset, validation, local scoping, envp vector");

    /* Test S5 Aliases */
    alias_init();
    assert(alias_set("ll", "ls -l") == 0);
    assert(!strcmp(alias_get("ll"), "ls -l"));

    char abuf[512];
    assert(alias_expand_line("ll /bin", abuf, sizeof(abuf)) && !strcmp(abuf, "ls -l /bin"));
    assert(!alias_expand_line("'ll' /bin", abuf, sizeof(abuf))); /* Quoted command word is not expanded */
    assert(!alias_expand_line("\"ll\" /bin", abuf, sizeof(abuf)));

    /* Self-recursion: alias ls='ls -F' expands once */
    alias_set("ls", "ls -F");
    assert(alias_expand_line("ls /tmp", abuf, sizeof(abuf)) && !strcmp(abuf, "ls -F /tmp"));

    /* Cycle detection: a -> b -> a terminates without infinite loop */
    alias_set("a", "b");
    alias_set("b", "a");
    assert(alias_expand_line("a arg", abuf, sizeof(abuf)));

    /* POSIX Trailing space continuation: alias with trailing blank makes next word eligible */
    alias_set("sudo", "sudo ");
    alias_set("ll", "ls -l");
    /* Note: 'ls' is currently aliased to 'ls -F', so 'll' expands to 'ls -F -l' (chained!) */
    assert(alias_expand_line("sudo ll /bin", abuf, sizeof(abuf)) && !strcmp(abuf, "sudo ls -F -l /bin"));

    /* After unsetting 'ls', expands directly to 'sudo ls -l /bin' */
    alias_unset("ls");
    assert(alias_expand_line("sudo ll /bin", abuf, sizeof(abuf)) && !strcmp(abuf, "sudo ls -l /bin"));

    /* Quoting and backslash suppression: \sudo suppresses expansion */
    assert(!alias_expand_line("\\sudo ll", abuf, sizeof(abuf)));

    /* Suppressed second word: sudo \ll expands sudo, preserves \ll */
    assert(alias_expand_line("sudo \\ll", abuf, sizeof(abuf)) && !strcmp(abuf, "sudo \\ll"));

    /* Command separators reset eligibility: ll; ll expands both */
    assert(alias_expand_line("ll; ll", abuf, sizeof(abuf)) && !strcmp(abuf, "ls -l; ls -l"));
    assert(alias_expand_line("ll && ll", abuf, sizeof(abuf)) && !strcmp(abuf, "ls -l && ls -l"));

    /* Verify parser / lexer distinguishes \cmd: produces unescaped word with QUOTE_ESCAPED flag */
    lexer_t esc_lex;
    token_t esc_tok;
    lexer_init(&esc_lex, "\\ll /bin");
    assert(lexer_next(&esc_lex, &esc_tok) == TOK_WORD);
    assert(!strcmp(esc_tok.value, "ll")); /* word value is unescaped command name */
    assert(esc_tok.quote_flags[0] == QUOTE_ESCAPED); /* distinct from normal unquoted token */
    assert(esc_tok.has_quotes);

    assert(alias_unset("ll") == 0);
    assert(alias_unset("sudo") == 0);
    assert(alias_get("ll") == NULL);

    puts("PASS shell aliases: definitions, lookup, expansion, quote suppression, trailing space, self-recursion, cycle bounds");

    /* Test S5 Globbing */
    assert(glob_match("*.c", "main.c"));
    assert(!glob_match("*.c", "main.h"));
    assert(glob_match("*.c", ".c"));
    assert(glob_match("h?llo", "hello"));
    assert(!glob_match("h?llo", "hllo"));
    assert(glob_match("[a-z]*", "test.txt"));
    assert(!glob_match("[a-z]*", "123.txt"));
    assert(glob_match("[!0-9]*", "test"));
    assert(!glob_match("[!0-9]*", "9test"));
    assert(glob_match("*.[ch]", "foo.c"));
    assert(glob_match("*.[ch]", "foo.h"));
    assert(!glob_match("*.[ch]", "foo.o"));

    puts("PASS shell globbing: *, ?, ranges, negated ranges");

    /* Test S5 Expansion Pipeline */
    vars_set("USER", "fortress", false);
    vars_set("WORDS", "alpha beta gamma", false);
    vars_set("EMPTY_VAR", "", false);

    parse_tree_t ptree;
    expanded_cmd_t ecmd;

    /* 1. Parameter expansion and quote suppression */
    assert(parser_parse("echo $USER \"$USER\" '$USER'", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 0, &ecmd) == 0);
    assert(ecmd.argc == 4);
    assert(!strcmp(ecmd.argv[0], "echo"));
    assert(!strcmp(ecmd.argv[1], "fortress"));
    assert(!strcmp(ecmd.argv[2], "fortress"));
    assert(!strcmp(ecmd.argv[3], "$USER")); /* Single quotes suppress expansion */

    /* 2. Word splitting on unquoted expansion */
    assert(parser_parse("cmd $WORDS", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 0, &ecmd) == 0);
    assert(ecmd.argc == 4);
    assert(!strcmp(ecmd.argv[0], "cmd"));
    assert(!strcmp(ecmd.argv[1], "alpha"));
    assert(!strcmp(ecmd.argv[2], "beta"));
    assert(!strcmp(ecmd.argv[3], "gamma"));

    /* 3. Double quotes prevent word splitting */
    assert(parser_parse("cmd \"$WORDS\"", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 0, &ecmd) == 0);
    assert(ecmd.argc == 2);
    assert(!strcmp(ecmd.argv[0], "cmd"));
    assert(!strcmp(ecmd.argv[1], "alpha beta gamma"));

    /* 4. Empty variable handling: unquoted discarded, quoted preserved */
    assert(parser_parse("cmd $EMPTY_VAR \"$EMPTY_VAR\" \"\"", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 0, &ecmd) == 0);
    assert(ecmd.argc == 3);
    assert(!strcmp(ecmd.argv[0], "cmd"));
    assert(!strcmp(ecmd.argv[1], ""));
    assert(!strcmp(ecmd.argv[2], ""));

    /* 5. Tilde expansion */
    vars_set("HOME", "/root", false);
    assert(parser_parse("ls ~/dir '~'", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 0, &ecmd) == 0);
    assert(ecmd.argc == 3);
    assert(!strcmp(ecmd.argv[0], "ls"));
    assert(!strcmp(ecmd.argv[1], "/root/dir"));
    assert(!strcmp(ecmd.argv[2], "~")); /* Quoted tilde is literal */

    /* 6. Special parameters $? and $$ */
    assert(parser_parse("echo $? $$", &ptree) == PARSE_OK);
    assert(expand_command(&ptree.cmds[0], 42, &ecmd) == 0);
    assert(ecmd.argc == 3);
    assert(!strcmp(ecmd.argv[0], "echo"));
    assert(!strcmp(ecmd.argv[1], "42"));
    assert(!strcmp(ecmd.argv[2], "1"));

    puts("PASS shell expansion: parameter expansion, quoting suppression, word splitting, empty preservation, tilde, $?");

    /* Test S6 Redirection Action Builder (Step 4A) */
    spawn_fd_action_t actions[MAX_SPAWN_ACTIONS];
    uint32_t act_count = 0;
    char target_paths[MAX_SPAWN_ACTIONS][VFS_MAX_PATH];

    /* 1. Input redirection */
    assert(parser_parse("cat < input.txt", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 1);
    assert(actions[0].type == SPAWN_FD_ACTION_OPEN);
    assert(actions[0].dst_fd == 0);
    assert(actions[0].flags == VFS_O_RDONLY);
    assert(!strcmp((const char *)actions[0].path, "input.txt"));

    /* 2. Output and Append redirection */
    assert(parser_parse("echo foo > out.txt >> app.txt", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 2);
    assert(actions[0].type == SPAWN_FD_ACTION_OPEN);
    assert(actions[0].dst_fd == 1);
    assert(actions[0].flags == (VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC));
    assert(!strcmp((const char *)actions[0].path, "out.txt"));
    assert(actions[1].type == SPAWN_FD_ACTION_OPEN);
    assert(actions[1].dst_fd == 1);
    assert(actions[1].flags == (VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND));
    assert(!strcmp((const char *)actions[1].path, "app.txt"));

    /* 3. Stderr and Duplication ordering: >out 2>&1 */
    assert(parser_parse("cmd > out.txt 2>&1", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 2);
    assert(actions[0].type == SPAWN_FD_ACTION_OPEN && actions[0].dst_fd == 1);
    assert(actions[1].type == SPAWN_FD_ACTION_DUP2 && actions[1].dst_fd == 2 && actions[1].src_fd == 1);

    /* 4. Inverted duplication ordering: 2>&1 >out */
    assert(parser_parse("cmd 2>&1 > out.txt", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 2);
    assert(actions[0].type == SPAWN_FD_ACTION_DUP2 && actions[0].dst_fd == 2 && actions[0].src_fd == 1);
    assert(actions[1].type == SPAWN_FD_ACTION_OPEN && actions[1].dst_fd == 1);

    /* Input duplication uses the same ordered DUP2 action as output. */
    assert(parser_parse("cmd 2<&0", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 1);
    assert(actions[0].type == SPAWN_FD_ACTION_DUP2 && actions[0].dst_fd == 2 && actions[0].src_fd == 0);

    /* 5. Close descriptor: >&- and 2<&- */
    assert(parser_parse("cmd >&- 2<&-", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 2);
    assert(actions[0].type == SPAWN_FD_ACTION_CLOSE && actions[0].dst_fd == 1);
    assert(actions[1].type == SPAWN_FD_ACTION_CLOSE && actions[1].dst_fd == 2);

    /* 6. Target expansion and ambiguous redirect */
    vars_set("OUT_FILE", "expanded.txt", false);
    vars_set("AMBIG_VAR", "word1 word2", false);
    assert(parser_parse("cmd > $OUT_FILE", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) == 0);
    assert(act_count == 1);
    assert(!strcmp((const char *)actions[0].path, "expanded.txt"));

    assert(parser_parse("cmd > $AMBIG_VAR", &ptree) == PARSE_OK);
    assert(redir_build_spawn_actions(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0,
                                     actions, &act_count, target_paths) != 0);

    /* Test S6 Parent Builtin Scoped Redirection (Step 4C & 4D) */
    redir_scope_t parent_scope;

    /* 1. Basic parent stdout and stderr redirection */
    mock_reset_fds();
    assert(s_mock_fds[1] == 2 && s_mock_fds[2] == 3);
    assert(parser_parse("echo hello > out.txt 2>&1", &ptree) == PARSE_OK);
    assert(redir_apply_parent(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0, &parent_scope) == 0);
    assert(parent_scope.count == 2);
    /* Descriptors 1 and 2 redirected */
    assert(s_mock_fds[1] != 2);
    assert(s_mock_fds[2] == s_mock_fds[1]);
    /* Saved copies exist in high slots >= 20 with CLOEXEC */
    assert(parent_scope.saved[0].orig_fd == 1);
    assert(parent_scope.saved[0].saved_fd >= 20);
    assert(s_mock_flags[parent_scope.saved[0].saved_fd] == FD_CLOEXEC);
    assert(parent_scope.saved[1].orig_fd == 2);
    assert(parent_scope.saved[1].saved_fd >= 20);
    assert(s_mock_flags[parent_scope.saved[1].saved_fd] == FD_CLOEXEC);

    /* Restore and verify original descriptors restored and saved descriptors closed */
    redir_restore_parent(&parent_scope);
    assert(s_mock_fds[1] == 2);
    assert(s_mock_fds[2] == 3);
    for (int k = 20; k < 32; k++) {
        assert(s_mock_fds[k] == -1);
    }
    assert(parent_scope.count == 0);

    /* 2. Parent redirection of originally unopened descriptor */
    mock_reset_fds();
    s_mock_fds[5] = -1;
    assert(parser_parse("echo foo 5> out.txt", &ptree) == PARSE_OK);
    assert(redir_apply_parent(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0, &parent_scope) == 0);
    assert(s_mock_fds[5] != -1);
    assert(parent_scope.saved[0].orig_fd == 5);
    assert(parent_scope.saved[0].was_open == false);
    assert(parent_scope.saved[0].saved_fd == -1);
    redir_restore_parent(&parent_scope);
    assert(s_mock_fds[5] == -1); /* Restored to closed! */

    /* 3. Automatic rollback on failure without executing command */
    mock_reset_fds();
    assert(s_mock_fds[1] == 2 && s_mock_fds[2] == 3);
    assert(parser_parse("echo foo > out.txt 2> /nonexistent", &ptree) == PARSE_OK);
    assert(redir_apply_parent(ptree.cmds[0].redirs, ptree.cmds[0].redir_count, 0, &parent_scope) != 0);
    /* Original fds must be completely restored */
    assert(s_mock_fds[1] == 2);
    assert(s_mock_fds[2] == 3);
    for (int k = 20; k < 32; k++) {
        assert(s_mock_fds[k] == -1);
    }

    puts("PASS shell redirections: spawn actions, parent save/apply/restore, open/dup/close, lexical order, target expansion, ambiguous rejection");
    test_pipelines();
    return 0;
}
