#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lineedit.h"
#include "lexer.h"
#include "parser.h"

static line_editor_t e;
static void feed(const char *s) { while(*s) lineedit_byte(&e,(unsigned char)*s++); }

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
    assert(parser_parse("|| foo", &tree) == PARSE_SYNTAX_ERROR);
    assert(parser_parse("!", &tree) == PARSE_SYNTAX_ERROR);

    /* 6. Incomplete input returns PARSE_INCOMPLETE */
    assert(parser_parse("echo 'unterminated", &tree) == PARSE_INCOMPLETE);
    assert(parser_parse("echo foo &&", &tree) == PARSE_INCOMPLETE);

    puts("PASS shell parser: AST building, precedence, operators, negation, syntax errors");
    return 0;
}
