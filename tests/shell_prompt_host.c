#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/shell/ui.c"
static char output[4096];
static size_t used;
static uint32_t actor;
static bool unavailable;
static const char *host="dell-5590\n";
long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr==SYS_OPEN) return !strcmp((const char *)a,"/etc/passwd") ? 3 : 4;
    if (nr==SYS_READ) {
        const char *bytes=a==3 ? "root:x:0:0::/root:/bin/shell\nfabio:x:1000:1000::/home/fabio:/bin/shell\n" : host;
        size_t n=strlen(bytes);assert(n<=c);memcpy((void *)b,bytes,n);return (long)n;
    }
    if (nr == SYS_GETRESUID) {
        if (unavailable) return SYSCALL_ESRCH;
        *(uint32_t *)a=actor; *(uint32_t *)b=actor; *(uint32_t *)c=actor;
    }
    return 0;
}
size_t length(const char *s) { return strlen(s); }
bool equal(const char *a,const char *b) { return strcmp(a,b)==0; }
long write_bytes_fd(int fd,const char *s,size_t n) {
    (void)fd; assert(used+n<sizeof(output)); memcpy(output+used,s,n);
    used+=n; output[used]=0; return (long)n;
}
void puts_fd(int fd,const char *s) { (void)write_bytes_fd(fd,s,strlen(s)); }
static void render(uint32_t uid,unsigned cols,const char *expected) {
    actor=uid; used=0; output[0]=0; term.cols=cols; term.mode=TERM_PLAIN;
    shell_set_prompt_state(0,"/run/user/1000"); paint();
    assert(strstr(output,expected));
}
int main(void) {
    render(1000,80,"fabio@dell-5590:/run/user/1000$ ");
    render(0,80,"root@dell-5590:/run/user/1000# ");
    render(1000,80,"fabio@dell-5590:/run/user/1000$ ");
    render(0,20,"# "); render(1000,20,"$ ");
    unavailable=true; render(0,80,"$ ");
    unavailable=false;host="bad\033name\n";render(1000,80,"fabio@fortress:/run/user/1000$ ");
    used=0;shell_set_prompt_state(0,"/tmp");paint();assert(strstr(output,"fabio@fortress:/tmp$ "));
    used=0;term.cols=50;shell_set_prompt_state(0,"/very/long/path/that/will/not/fit/the/terminal/width/end");paint();
    assert(strstr(output,"fabio@fortress:") && strstr(output,"end$ "));
    fprintf(stderr,"PASS username/hostname/path prompt, UID transitions, narrow terminal and malformed hostname fallback\n");
    return 0;
}
