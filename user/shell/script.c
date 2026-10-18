#include "script.h"
#include "io.h"
#include "lineedit.h"
bool shell_script_mode;
volatile unsigned script_interrupted;
static char unit[LINE_CAP];
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
        if(used==sizeof(unit)-1) {diagnostic(name,start,"command exceeds 4096 bytes");return 2;}
        unit[used++]=(char)c;unit[used]=0;
        if(c!='\n') continue;
        unit[used-1]=0;
        int result=eval(unit);line++;
        if(result==-2) {diagnostic(name,start,"syntax error");return 2;}
        if(result<0) unit[used-1]='\n';
        if(result>=0) {status=result;used=0;start=line;}
    }
}
