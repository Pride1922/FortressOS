#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PERMISSIONS_CLI_HOST_TEST
#include "../user/tools/sudo.c"
static bool authorized=true,empty=true,verify_ok=true,transition_fail;
static unsigned transitions,spawns,reads,closes;
static const char *input;static char output[2048];static size_t written;
bool db_load(userdb_t *out,bool shadow) {
    assert(shadow);memset(out,0,sizeof(*out));out->nusers=2;out->ngroups=2;out->nshadows=1;
    out->users[0]=(db_user_t){.uid=0,.gid=0,.name="root"};
    out->users[1]=(db_user_t){.uid=1000,.gid=1000,.name="operator"};
    out->groups[0]=(db_group_t){.gid=0,.name="root"};
    out->groups[1]=(db_group_t){.gid=10,.name="wheel"};
    if (authorized) memcpy(out->groups[1].members,"operator",9);
    memcpy(out->shadows[0].name,"operator",9);if (!empty) memcpy(out->shadows[0].hash,"!",2);
    return true;
}
/* Pure crypt is independently tested by test-perm-db-host. */
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_GETRESUID) {*(uint32_t *)a=1000;*(uint32_t *)b=0;*(uint32_t *)c=0;return 0;}
    if (nr==SYS_OPEN) {assert(!strcmp((char *)a,"/dev/tty"));return 7;}
    if (nr==SYS_TERMCTL) {assert(a==TERM_ISATTY && b==7);return 1;}
    if (nr==SYS_READ) {assert(a==7);reads++;if (!*input) return SYSCALL_EINTR;*(char *)b=*input++;return 1;}
    if (nr==SYS_WRITE) {assert(written+c<sizeof(output));memcpy(output+written,(void *)b,c);written+=c;return c;}
    if (nr==SYS_CLOSE) {assert(a==7);closes++;return 0;}
    if (nr==SYS_SETGROUPS) {transitions++;assert(a==1 && *(uint32_t *)b==0);return 0;}
    if (nr==SYS_SPAWN_EXT) {
        spawns++;assert(!strcmp((char *)a,"/bin/id"));const spawn_opts_t *o=(void *)b;
        const char *const *e=(void *)o->envp;assert(!strcmp(e[0],"PATH=/bin") && !strcmp(e[1],"HOME=/root"));
        assert(!strcmp(e[2],"USER=root") && !strcmp(e[5],"TERM=test"));assert(!e[6]);return 99;
    }
    if (nr==SYS_WAIT) {assert(a==99);*(uint64_t *)b=37;return 99;}
    assert(!"unexpected syscall");return -1;
}
long permission_call4(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    assert(nr==SYS_SETRESGID || nr==SYS_SETRESUID);assert(!a && !b && !c && !d);
    transitions++;return transition_fail ? SYSCALL_EPERM:0;
}
int main(void) {
    (void)verify_ok;char *args[]={"sudo","id",NULL};
    uintptr_t e[]={(uintptr_t)"PATH=/evil",(uintptr_t)"TERM=test",0,23,1,0,0};
    input="secret\n";assert(sudo_main(2,args,(const char *const *)e)==37);
    assert(transitions==3 && spawns==1 && !reads && closes==1);
    authorized=false;transitions=spawns=0;assert(sudo_main(2,args,(const char *const *)e)==1 && !transitions && !spawns);
    authorized=true;empty=false;assert(sudo_main(2,args,(const char *const *)e)==1 && !transitions && !spawns);
    empty=true;transition_fail=true;assert(sudo_main(2,args,(const char *const *)e)==1 && !spawns);
    char password[DB_PASSWORD_MAX+1]={0};input="secret\n";written=0;
    assert(password_line(7,password) && !strcmp(password,"secret") && !written);
    char over[DB_PASSWORD_MAX+8];memset(over,'a',sizeof(over));over[sizeof(over)-2]='\n';over[sizeof(over)-1]=0;input=over;
    assert(!password_line(7,password) && !*input);
    input="x";assert(!password_line(7,password));for (unsigned i=0;i<sizeof(password);i++) assert(!password[i]);
    puts("PASS actual sudo wheel/locked/passwordless/credential failure/minimal environment/status; terminal no-echo/drain/wipe adapters");
}
