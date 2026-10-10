#include "common.h"
static int number(uint32_t value) {
    char text[12];size_t n=tool_format_u64(text,value);return tool_write("id",text,n);
}
static int label(const char *text) {return tool_write("id",text,tool_length(text));}
int id_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) return label("Usage: id (numeric IDs and supplementary groups)\n");
    if (argc!=1) return tool_error("id","unexpected operand",NULL);
    uint32_t uid,euid,suid,gid,egid,sgid,groups[16];
    long r=tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid);
    if (!r) r=tool_syscall(SYS_GETRESGID,(uintptr_t)&gid,(uintptr_t)&egid,(uintptr_t)&sgid);
    if (r<0) return tool_error("id","cannot read identity",NULL);
    long count=tool_syscall(SYS_GETGROUPS,16,(uintptr_t)groups,0);
    if (count<0 || count>16) return tool_error("id","cannot read groups",NULL);
    if (label("uid=") || number(uid) || label(" gid=") || number(gid)) return 1;
    if (euid!=uid && (label(" euid=") || number(euid))) return 1;
    if (egid!=gid && (label(" egid=") || number(egid))) return 1;
    if (suid!=euid && (label(" suid=") || number(suid))) return 1;
    if (sgid!=egid && (label(" sgid=") || number(sgid))) return 1;
    if (label(" groups=") || number(egid)) return 1;
    for (long i=0;i<count;i++) if (groups[i]!=egid && (label(",") || number(groups[i]))) return 1;
    return label("\n");
}
