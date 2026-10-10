#include "common.h"
#include "userdb.h"
int whoami_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) return tool_write("whoami","Usage: whoami\n",14);
    if (argc!=1) return tool_error("whoami","unexpected operand",0);
    uint32_t r,e,s;static userdb_t db;
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s)<0 || !db_load(&db,false)) return tool_error("whoami","cannot read identity",0);
    const db_user_t *u=db_user_id(&db,e);if (!u) return tool_error("whoami","unknown uid",0);
    return tool_write("whoami",u->name,tool_length(u->name)) || tool_write("whoami","\n",1);
}
