#include "common.h"
#include "../permissions_cli.h"
int chown_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) {
        const char *s="Usage: chown UID[:GID] FILE... (numeric IDs)\n";
        return tool_write("chown",s,tool_length(s));
    }
    if (argc<3) return tool_error("chown","expected owner and file",NULL);
    const char *s=argv[1];size_t n=tool_length(s),colon=0;
    while (colon<n && s[colon]!=':') colon++;
    uint32_t uid=0,gid=0,flags=CHOWN_KEEP_GID;
    if (!colon) flags|=CHOWN_KEEP_UID;
    else if (!permission_number(s,colon,10,UINT32_MAX,&uid)) return tool_error("chown","invalid UID",s);
    if (colon<n && colon+1<n) {
        if (!permission_number(s+colon+1,n-colon-1,10,UINT32_MAX,&gid)) return tool_error("chown","invalid GID",s);
        flags&=~CHOWN_KEEP_GID;
    }
    if (flags==(CHOWN_KEEP_UID|CHOWN_KEEP_GID)) return tool_error("chown","missing UID and GID",s);
    int status=0;
    for (int i=2;i<argc;i++) if (permission_call4(SYS_CHOWN,(uintptr_t)argv[i],uid,gid,flags)<0)
        status=tool_error("chown","cannot change owner",argv[i]);
    return status;
}
