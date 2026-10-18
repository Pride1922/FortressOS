#include "script.h"
#include "io.h"
#include "lineedit.h"
#include "control.h"
bool shell_script_mode;
volatile unsigned script_interrupted;
static char unit[CONTROL_SOURCE_CAP];
static void diagnostic(const char *name,size_t line,const char *message) {
    puts_err(name);puts_err(":");
    char digits[24];unsigned n=0;
    do {digits[n++]=(char)('0'+line%10);line/=10;} while(line);
    while(n) {char c[2]={digits[--n],0};puts_err(c);}
    puts_err(": ");puts_err(message);puts_err("\n");
}
int script_run(int fd,const char *text,const char *name,int (*eval)(const char *)) {
    size_t used=0,offset=0,line=1,start=1;int status=0;bool cr=false;
    for (;;) {
        if (script_interrupted) return 128+(int)script_interrupted;
        unsigned char c=0;long n;
        if(text) {c=(unsigned char)text[offset];n=c ? 1 : 0;if(n) offset++;}
        else n=call(SYS_READ,fd,(uintptr_t)&c,1);
        if(n==SYSCALL_EINTR) continue;
        if(n<0) {diagnostic(name,line,"script read failed");return 2;}
        if(!n) {
            if(used) {
                unit[used]=0;int result=eval(unit);
                if(result<0) {diagnostic(name,start,result==-1 ? "unexpected EOF in command" : "syntax error");return 2;}
                status=result;
            }
            return status;
        }
        if(!c) {diagnostic(name,line,"NUL byte in script");return 2;}
        if(c=='\n' && cr) {cr=false;continue;}
        cr=c=='\r';if(cr) c='\n';
        if(used==sizeof(unit)-1) {diagnostic(name,start,"compound unit exceeds 16 KiB");return 2;}
        unit[used++]=(char)c;unit[used]=0;
        if(c!='\n') continue;
        unit[used-1]=0;
        int result=eval(unit);line++;
        if(result==-2) {diagnostic(name,start,"syntax error");return 2;}
        if(result<0) unit[used-1]='\n';
        if(result>=0) {status=result;used=0;start=line;}
    }
}

void script_reader_init(script_reader_t *r) {
    r->used=0;r->line=r->start=1;r->cr=r->eof=false;
}
int script_reader_next(script_reader_t *r,int fd,const char *name,
                       int (*compile)(const char *)) {
    if(r->eof) return 0;
    for(;;) {
        if(script_interrupted) return -2;
        unsigned char c=0;long n=call(SYS_READ,fd,(uintptr_t)&c,1);
        if(n==SYSCALL_EINTR) continue;
        if(n<0) {diagnostic(name,r->line,"script read failed");return -2;}
        if(!n) {
            r->eof=true;if(!r->used) return 0;
            r->unit[r->used]=0;int result=compile(r->unit);
            if(result<0) {diagnostic(name,r->start,result==-1 ? "unexpected EOF in command" : "syntax error");return -2;}
            r->used=0;return 1;
        }
        if(!c) {diagnostic(name,r->line,"NUL byte in script");return -2;}
        if(c=='\n' && r->cr) {r->cr=false;continue;}
        r->cr=c=='\r';if(r->cr) c='\n';
        if(r->used==sizeof(r->unit)-1) {diagnostic(name,r->start,"compound unit exceeds 16 KiB");return -2;}
        r->unit[r->used++]=(char)c;r->unit[r->used]=0;
        if(c!='\n') continue;
        r->unit[r->used-1]=0;int result=compile(r->unit);r->line++;
        if(result==-2) {diagnostic(name,r->start,"syntax error");return -2;}
        if(result<0) {r->unit[r->used-1]='\n';continue;}
        r->used=0;r->start=r->line;return 1;
    }
}
