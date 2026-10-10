#include "common.h"
#include "../permissions_cli.h"
int chmod_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) {
        const char *s="Usage: chmod OCTAL FILE...\n";return tool_write("chmod",s,tool_length(s));
    }
    uint32_t mode;
    if (argc<3 || !permission_number(argv[1],tool_length(argv[1]),8,07777,&mode))
        return tool_error("chmod","expected octal mode and file",NULL);
    int status=0;
    for (int i=2;i<argc;i++) if (tool_syscall(SYS_CHMOD,(uintptr_t)argv[i],mode,0)<0)
        status=tool_error("chmod","cannot change mode",argv[i]);
    return status;
}
