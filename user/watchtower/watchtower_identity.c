#include "watchtower_identity.h"

static void id_sleep_5ms(void) {
#ifndef WATCHTOWER_HOST_TEST
#ifndef __linux__
    timespec_t ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    long ret;
    __asm__ volatile (
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "movq %7, %%r9\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "g"((long)SYS_NANOSLEEP), "g"((uintptr_t)&ts), "g"(0UL), "g"(0UL), "g"(0UL), "g"(0UL), "g"(0UL)
        : "rcx", "r11", "memory", "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );
    (void)ret;
#endif
#endif
}
static bool equal(const char *a,const char *b) {
    while (*a && *a==*b) {a++;b++;} return *a==*b;
}
static bool numeric(const char *s,uint32_t *out) {
    uint32_t n=0;
    if (!*s) return false;
    for (;*s;s++) {
        if (*s<'0' || *s>'9' || n>(UINT32_MAX-(unsigned)(*s-'0'))/10) return false;
        n=n*10+(unsigned)(*s-'0');
    }
    *out=n;return true;
}
static bool digits(const char *s) {
    if (!*s) return false;
    for (;*s;s++) if (*s<'0' || *s>'9') return false;
    return true;
}
bool watchtower_resolve_identity(const userdb_t *db,const char *user,
                                 const char *group,spawn_identity_t *out) {
    if (!db || db->nusers>DB_MAX_RECORDS || db->ngroups>DB_MAX_RECORDS ||
        !user || !*user || !group || !*group || !out) return false;
    uint32_t id;
    /* All-digit selectors are IDs, never a fallback to numeric account names. */
    if ((digits(user) && !numeric(user,&id)) || (digits(group) && !numeric(group,&id))) return false;
    const db_user_t *u=numeric(user,&id) ? db_user_id(db,id) : db_user_name(db,user);
    const db_group_t *g=NULL;
    if (numeric(group,&id)) g=db_group_id(db,id);
    else for (unsigned i=0;i<db->ngroups;i++)
        if (equal(group,db->groups[i].name)) {g=&db->groups[i];break;}
    if (!u || !g || !db_group_id(db,u->gid)) return false;
    spawn_identity_t next={.uid=u->uid,.gid=g->gid};
    unsigned count=0;
    if (!db_groups(db,u,next.groups,&count)) return false;
    /* Explicit Group replaces the account's primary group, retaining only its
     * database supplementary memberships; no supervisor groups are inherited. */
    next.groups[0]=g->gid;
    unsigned n=1;
    for (unsigned i=1;i<count;i++) {
        bool duplicate=false;
        for (unsigned j=0;j<n;j++) if (next.groups[j]==next.groups[i]) duplicate=true;
        if (!duplicate) next.groups[n++]=next.groups[i];
    }
    for (unsigned i=n;i<16;i++) next.groups[i]=0;
    next.ngroups=n;*out=next;return true;
}
bool watchtower_prepare_identity(const char *user,const char *group,spawn_identity_t *out) {
    static userdb_t db;
    spawn_identity_t next;
    if (!out || !user || !*user || !group || !*group) return false;

    bool loaded = false;
    for (int retry = 0; retry < 10; retry++) {
        if (db_load(&db, false)) {
            loaded = true;
            break;
        }
        if (!db_busy()) break;
        id_sleep_5ms();
    }
    if (!loaded) return false;

    if (!watchtower_resolve_identity(&db, user, group, &next))
        return false;

    bool check_ok = false;
    for (int retry = 0; retry < 10; retry++) {
        if (db_check()) {
            check_ok = true;
            break;
        }
        id_sleep_5ms();
    }
    if (!check_ok) return false;

    *out = next;
    return true;
}
