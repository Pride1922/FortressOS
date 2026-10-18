#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "control.h"
#include "parser.h"
#include "vars.h"
#include "expand.h"
#include "test_expr.h"
static parse_tree_t tree;
static expanded_cmd_t args;
static char records[256][256];
static unsigned count;
static volatile unsigned interrupted;
static bool yielded;
long call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)c;
    if(nr==SYS_STAT) {
        if(!strcmp((const char *)a,"/file")) {((vfs_stat_t *)b)->type=VFS_FILE;return 0;}
        if(!strcmp((const char *)a,"/dir")) {((vfs_stat_t *)b)->type=VFS_DIRECTORY;return 0;}
        if(!strcmp((const char *)a,"/denied")) return SYSCALL_EACCES;
        return SYSCALL_ENOENT;
    }
    return SYSCALL_ENOSYS;
}
size_t length(const char *s) {return strlen(s);}
bool equal(const char *a,const char *b) {return !strcmp(a,b);}
long puts_err(const char *s) {(void)s;return 0;}
void put_dec(size_t n) {(void)n;}
void write_bytes(const char *s,size_t n) {(void)s;(void)n;}
static int evaluate(const char *text,int status) {
    assert(parser_parse(text,&tree)==PARSE_OK);
    assert(tree.cmd_count==1);
    assert(!expand_command_checked(&tree.cmds[0],status,&args));
    char **v=args.argv;int n=args.argc;
    if(!strcmp(v[0],"true")) return 0;
    if(!strcmp(v[0],"false")) return 1;
    if(!strcmp(v[0],"test") || !strcmp(v[0],"[")) return shell_test(n,(const char *const *)v);
    if(!strcmp(v[0],"break") || !strcmp(v[0],"continue")) return control_request(!strcmp(v[0],"continue"),n,v);
    if(!strcmp(v[0],"setvar")) {assert(n==3);return vars_set(v[1],v[2],false);}
    if(!strcmp(v[0],"interrupt")) {interrupted=SIGINT;return 0;}
    if(!strcmp(v[0],"yield")) {
        if(!yielded) {yielded=true;control_stop=CONTROL_SOURCE_PENDING;return 0;}
        yielded=false;return status;
    }
    if(!strcmp(v[0],"returning")) {control_stop=CONTROL_RETURN_PENDING;return 7;}
    assert(!strcmp(v[0],"record"));assert(count<256 && n==2);
    memcpy(records[count++],v[1],strlen(v[1])+1);return 0;
}
static int run(const char *text) {assert(control_compile(text)==0);return control_execute(0,evaluate,&interrupted);}
int main(void) {
    vars_init();char *v[]={"a b",""};assert(vars_positionals("test",2,v));
    assert(!control_select(CONTROL_MAX_SOURCES+1));
    assert(control_compile("for x in a b; do yield; record $x; done")==0);
    count=0;
    for(unsigned round=0;round<2;round++) {
        assert(control_execute(0,evaluate,&interrupted)==CONTROL_YIELD);
        assert(control_select(1));
        assert(control_compile("for y in c d; do record $y; done; returning; record BAD")==0);
        assert(control_execute(0,evaluate,&interrupted)==7);
        assert(control_select(0));
        if(!round) assert(!strcmp(records[0],"c"));
    }
    assert(control_execute(5,evaluate,&interrupted)==0 && count==6);
    assert(!strcmp(records[2],"a") && !strcmp(records[5],"b"));
    count=0;
    assert(run("if false; then record BAD; elif true; then record GOOD; else record BAD; fi")==0);
    assert(count==1 && !strcmp(records[0],"GOOD"));
    count=0;assert(run("if false; then record BAD; fi")==0 && !count);
    assert(run("if true; then false; fi")==1);
    count=0;assert(run("for x in a 'b c' ''; do record \"$x\"; done")==0 && count==3);
    assert(!strcmp(records[1],"b c") && !strcmp(records[2],""));
    count=0;assert(run("for x; do record \"$x\"; done")==0 && count==2 && !strcmp(records[0],"a b"));
    count=0;assert(run("for x in; do record BAD; done")==0 && !count);
    count=0;assert(run("setvar X go; while test \"$X\" = go; do record ONCE; setvar X stop; done")==0 && count==1);
    count=0;assert(run("while true; do record ONCE; break; record BAD; done")==0 && count==1);
    count=0;assert(run("for x in a b c; do if [ \"$x\" = b ]; then continue; fi; record \"$x\"; done")==0 && count==2);
    assert(!strcmp(records[1],"c"));
    count=0;assert(run("for x in a b; do for y in c d; do record INNER; break 2; done; record BAD; done")==0 && count==1);
    count=0;assert(run("for x in a b; do for y in c d; do record INNER; continue 2; done; record BAD; done")==0 && count==2);
    assert(run("break")==1);
    count=0;assert(control_compile("if true; then record BAD; else else; fi")==-2 && !count);
    assert(control_compile("if true; then record BAD")==-1 && !count);
    assert(control_compile("for 123 in a; do record BAD; done")==-2);
    assert(control_compile("record x | record y")==0);
    assert(control_compile("if true; then record ok; fi | record bad")==-2);
    assert(control_compile("if true; then record ok; fi record bad")==-2);
    assert(control_compile("record BEFORE && if true; then record BAD; fi")==-2);
    assert(control_compile("record first &&\n# comment\nrecord second")==0);
    assert(control_compile("record first 2>&1\nif true; then record second; fi")==0);
    static char nested[2048];size_t used=0;
    for(unsigned i=0;i<9;i++) {const char *s="while true; do ";memcpy(nested+used,s,strlen(s));used+=strlen(s);}
    memcpy(nested+used,"true",5);assert(control_compile(nested)==-2);
    interrupted=0;assert(run("while true; do interrupt; done")==130);interrupted=0;
    static char budget[17000];used=0;
    for(unsigned i=0;i<260;i++) {memcpy(budget+used,"true;",5);used+=5;}
    budget[used]=0;assert(control_compile(budget)==-2 && strstr(control_error(),"256"));
    assert(control_execute(0,evaluate,&interrupted)==2);
    memset(budget,'x',sizeof(budget)-1);budget[sizeof(budget)-1]=0;
    assert(control_compile(budget)==-2 && strstr(control_error(),"16 KiB"));
    used=0;for(unsigned i=0;i<1030;i++) {memcpy(budget+used,"x ",2);used+=2;}
    budget[used]=0;assert(control_compile(budget)==-2 && strstr(control_error(),"1024"));
    char big[201];memset(big,'x',200);big[200]=0;assert(!vars_set("BIG",big,false));
    used=0;memcpy(budget,"for x in ",9);used=9;
    for(unsigned i=0;i<30;i++) {memcpy(budget+used,"\"$BIG\" ",7);used+=7;}
    const char *tail="; do record BAD; done";memcpy(budget+used,tail,strlen(tail)+1);
    count=0;assert(run(budget)==1 && !count);
    char too_long[300];memset(too_long,'x',299);too_long[299]=0;
    assert(vars_set("BIG",too_long,false)!=0 && strlen(vars_get("BIG"))==200);
    int64_t number;assert(test_integer("-9223372036854775808",&number) && number==(-9223372036854775807LL-1));
    assert(!test_integer("9223372036854775808",&number) && !test_integer("-9223372036854775809",&number));
    const char *zero[]={"test"},*one[]={"[","" ,"]"},*file[]={"test","-f","/file"},*dir[]={"test","-d","/dir"};
    const char *bad[]={"test","1","-eq","no"},*denied[]={"test","-e","/denied"};
    assert(shell_test(1,zero)==1 && shell_test(3,one)==1 && !shell_test(3,file) && !shell_test(3,dir));
    assert(shell_test(4,bad)==2 && shell_test(3,denied)==2);
    const char *minimum[]={"[","-9223372036854775808","-lt","0","]"};
    const char *maximum[]={"test","9223372036854775807","-gt","0"};
    const char *missing[]={"[","-n","x"},*empty[]={"test","-z",""};
    const char *string[]={"test","a","!=","b"},*negative[]={"test","!","-e","/missing"};
    const char *ambiguous[]={"test","x","-a","y"};
    assert(!shell_test(5,minimum) && !shell_test(4,maximum) && shell_test(3,missing)==2);
    assert(!shell_test(3,empty) && !shell_test(4,string) && !shell_test(4,negative) && shell_test(4,ambiguous)==2);
    puts("PASS control compiler/interpreter: branches, for/while, re-expansion, nested transfer, syntax-before-effects, budgets, cancellation and test grammar");
    return 0;
}
