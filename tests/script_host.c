#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "script.h"
#include "parser.h"
#include "syscall_abi.h"
static parse_tree_t tree;
static unsigned evaluated;
static char last[LINE_CAP];
static const unsigned char *input;
static size_t input_size,input_pos;
static bool fail_read;
long call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    assert(nr==SYS_READ && a==9 && c==1);
    if(fail_read) return SYSCALL_EIO;
    if(input_pos==input_size) return 0;
    *(unsigned char *)b=input[input_pos++];return 1;
}
long puts_err(const char *s) {(void)s;return 0;}
static int evaluate(const char *text) {
    enum parse_result r=parser_parse(text,&tree);
    if(r==PARSE_INCOMPLETE) return -1;
    if(r==PARSE_SYNTAX_ERROR) return -2;
    if(r==PARSE_EMPTY) return 0;
    evaluated++;memcpy(last,text,strlen(text)+1);return 7;
}
int main(void) {
    assert(script_run(0,"","empty",evaluate)==0);
    assert(script_run(0,"# comment\r\necho x\r\necho y","crlf",evaluate)==7 && evaluated==2);
    evaluated=0;
    assert(script_run(0,"echo a\\\nb\n","continuation",evaluate)==7 && evaluated==1);
    assert(strstr(last,"a\\\nb"));
    evaluated=0;
    assert(script_run(0,"echo 'hello\nworld'\n","quote",evaluate)==7 && evaluated==1);
    assert(script_run(0,"echo 'incomplete","bad",evaluate)==2);
    assert(script_run(0,"| echo bad\necho mustnotrun","bad",evaluate)==2 && evaluated==1);
    static char overflow[LINE_CAP+2];memset(overflow,'a',sizeof(overflow)-1);
    evaluated=0;assert(script_run(0,overflow,"bound",evaluate)==2 && !evaluated);
    const unsigned char nul[]={'e','c','h','o',0,'x'};
    input=nul;input_size=sizeof(nul);input_pos=0;
    assert(script_run(9,NULL,"nul",evaluate)==2 && !evaluated);
    input=(const unsigned char *)"echo read\n";input_size=10;input_pos=0;
    assert(script_run(9,NULL,"file",evaluate)==7 && evaluated==1);
    fail_read=true;assert(script_run(9,NULL,"io",evaluate)==2);
    script_interrupted=SIGTERM;assert(script_run(0,"echo no","cancel",evaluate)==128+SIGTERM);
    puts("PASS actual script reader: CRLF, continuation, multiline quote, incomplete EOF, syntax stop, overflow, NUL, read failure, cancellation");
    return 0;
}
