#include "common.h"
#include "userdb.h"
#include "../permissions_cli.h"
static char data[DB_FILE_MAX+1];
bool db_load(userdb_t *out,bool shadow) {
    static userdb_t next;next=(userdb_t){0};
    const char *paths[]={"/etc/passwd","/etc/group","/etc/shadow"};
    for (unsigned i=0;i<(shadow ? 3u : 2u);i++) {
        stat_ext_v1_t st;
        if (permission_call4(SYS_STAT_EXT,(uintptr_t)paths[i],(uintptr_t)&st,sizeof(st),1)<0 ||
            st.type!=VFS_FILE || (st.mode & VFS_S_IFMT)!=VFS_S_IFREG || st.uid || st.gid ||
            (st.mode & 07777)!=(i==2 ? 0600u : 0644u) || !(st.mnt_flags & VFS_MNT_RDONLY) ||
            st.file_size>DB_FILE_MAX) return false;
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)paths[i],VFS_O_RDONLY|VFS_O_CLOEXEC,0);
        if (fd<0) return false;
        size_t n=0;long r;
        do {r=tool_syscall(SYS_READ,fd,(uintptr_t)(data+n),sizeof(data)-n);
            if (r>0) n+=(size_t)r;
        } while (r>0 && n<sizeof(data));
        long closed=tool_syscall(SYS_CLOSE,fd,0,0);
        bool ok=r==0 && closed==0 && n<=DB_FILE_MAX &&
            (i==0 ? db_passwd(&next,data,n) : i==1 ? db_group(&next,data,n) : db_shadow(&next,data,n));
        db_wipe(data,sizeof(data));if (!ok) {db_wipe(&next,sizeof(next));return false;}
    }
    if (shadow && !db_validate(&next)) {db_wipe(&next,sizeof(next));return false;}
    *out=next;db_wipe(&next,sizeof(next));return true;
}
