/* Actual login terminal/runtime helpers with syscall adapters. Host-only;
 * no IRQ, scheduler or process-lifetime claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PERMISSIONS_CLI_HOST_TEST
#include "../user/tools/login.c"
static stat_ext_v1_t runtime_stat;
static bool missing;
static unsigned writes,mkdirs,chowns,chmods;
static const char *input;
static char output[1024];static size_t output_size;
bool db_load(userdb_t *out,bool shadow) {(void)out;(void)shadow;return false;}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_WRITE) {assert(a==1 || a==2);assert(output_size+c<sizeof(output));memcpy(output+output_size,(void *)b,c);output_size+=c;return c;}
    if (nr==SYS_INPUT_READ) {if (!*input) return SYSCALL_EINTR;*(char *)a=*input++;return 1;}
    if (nr==SYS_MKDIR) {assert(!strcmp((char *)a,"/run/user/1000") && b==0700);mkdirs++;writes++;missing=false;runtime_stat.uid=runtime_stat.gid=0;return 0;}
    if (nr==SYS_CHMOD) {assert(b==0700);chmods++;writes++;runtime_stat.mode=VFS_S_IFDIR|0700;return 0;}
    assert(!"unexpected syscall");return -1;
}
long permission_call4(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    (void)a;
    if (n==SYS_STAT_EXT) {assert(c==sizeof(runtime_stat) && d==1);if (missing) return SYSCALL_ENOENT;*(stat_ext_v1_t *)b=runtime_stat;return 0;}
    if (n==SYS_CHOWN) {assert(b==1000 && c==1000 && !d);runtime_stat.uid=b;runtime_stat.gid=c;chowns++;writes++;return 0;}
    assert(!"unexpected syscall4");return -1;
}
int main(void) {
    db_user_t u={.uid=1000,.gid=1000,.home="/run/user/1000"};char path[128];
    runtime_stat=(stat_ext_v1_t){.type=VFS_DIRECTORY,.mode=VFS_S_IFDIR|0700,.uid=1000,.gid=1000};
    assert(runtime(&u,path) && !writes && !strcmp(path,u.home));
    runtime_stat.uid=0;assert(!runtime(&u,path) && !writes);runtime_stat.uid=1000;
    runtime_stat.gid=44;assert(!runtime(&u,path) && !writes);runtime_stat.gid=1000;
    runtime_stat.mode=VFS_S_IFDIR|0777;assert(!runtime(&u,path) && !writes);runtime_stat.mode=VFS_S_IFDIR|0700;
    runtime_stat.type=VFS_FILE;assert(!runtime(&u,path) && !writes);runtime_stat.type=VFS_DIRECTORY;
    runtime_stat.mnt_flags=VFS_MNT_RDONLY;assert(!runtime(&u,path) && !writes);runtime_stat.mnt_flags=0;
    missing=true;assert(runtime(&u,path) && writes==3 && mkdirs==1 && chowns==1 && chmods==1);
    input="secret\n";char password[129];assert(line(password,sizeof(password),false) && !strcmp(password,"secret"));
    assert(output_size==1 && output[0]=='\n');db_wipe(password,sizeof(password));for (unsigned i=0;i<sizeof(password);i++) assert(!password[i]);
    input="very-long-input\n";char small[4];assert(!line(small,sizeof(small),false) && !*input);
    input="ab\bcd\n";assert(line(password,sizeof(password),false) && !strcmp(password,"acd"));
    input="partial";assert(!line(password,sizeof(password),false));for (unsigned i=0;i<sizeof(password);i++) assert(!password[i]);
    puts("PASS actual login helpers: no password echo, overflow drain, interrupted wipe; unsafe owner/group/type/mode/mount zero-effect rejection and privileged runtime creation");
}
