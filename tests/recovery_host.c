/* Actual recovery program and SHA256-crypt; host terminal/tick adapters only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <crypt.h>
#define db_wipe pure_db_wipe
#include "../user/tools/userdb.c"
#undef db_wipe
void db_wipe(void *,size_t);
#include "../user/tools/recovery.c"
static const char *input;
static char output[4096];static size_t written;
static uint64_t ticks;static unsigned wipes,delays;
static bool root_actor=true;
void db_wipe(void *p,size_t n) {
    pure_db_wipe(p,n);for (size_t i=0;i<n;i++) assert(!((unsigned char *)p)[i]);wipes++;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_GETRESUID) {*(uint32_t *)a=*(uint32_t *)b=*(uint32_t *)c=root_actor ? 0 : 1000;return 0;}
    if (nr==SYS_TERMCTL) {assert(a==TERM_ISATTY);return 1;}
    if (nr==SYS_SYSINFO) {sysinfo_t *s=(void *)a;s->uptime_ticks=ticks;ticks+=100;s->tick_hz=100;return 0;}
    if (nr==SYS_WRITE) {assert(written+c<sizeof(output));memcpy(output+written,(void *)b,c);written+=c;return c;}
    if (nr==SYS_INPUT_READ) {
        if (c==1000) {delays++;memset((void *)a,'x',b);return b;}
        assert(b==1);if (!*input) return SYSCALL_EINTR;*(char *)a=*input++;return 1;
    }
    assert(!"unexpected syscall");return -1;
}
static void reset(const char *text) {input=text;memset(output,0,sizeof(output));written=ticks=wipes=delays=0;}
int main(void) {
    char hash[96];const char *secret="Recovery fixture passphrase";
    const char *value=crypt(secret,"$5$rounds=100000$abcdefghijklmnop");assert(value && strlen(value)<sizeof(hash));memcpy(hash,value,strlen(value)+1);
    char *args[]={"recovery",hash};char text[1024];
    snprintf(text,sizeof(text),"wrong passphrase\n%s\n",secret);reset(text);
    assert(!recovery_main(2,args) && wipes==2 && delays>=2 && ticks>=300 && !strstr(output,secret) && strstr(output,"Recovery denied"));
    memset(text,'a',129);snprintf(text+129,sizeof(text)-129,"\n%s\n",secret);reset(text);
    assert(!recovery_main(2,args) && wipes==2 && delays>=2 && !strstr(output,secret));
    snprintf(text,sizeof(text),"\nshort\n%s\n",secret);reset(text);assert(!recovery_main(2,args) && wipes==3 && delays>=4);
    snprintf(text,sizeof(text),"bad\001input\n%s\n",secret);reset(text);assert(!recovery_main(2,args) && wipes==2);
    reset("partial");assert(recovery_main(2,args)==1 && wipes==1 && !strstr(output,"partial"));
    root_actor=false;reset(text);assert(recovery_main(2,args)==1 && !written && !wipes);
    puts("PASS actual independent recovery: crypt, no echo, empty/short/overflow/non-ASCII rejection, BSP deadlines, flood resistance and plaintext wipe");
}
