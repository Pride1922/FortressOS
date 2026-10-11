#include "common.h"
#include "../machine_config.h"
int hostname_main(int argc,char **argv) {
    if (argc<1 || argc>2) return tool_error("hostname","usage: hostname [NAME]",0);
    bool installed=machine_installed(tool_syscall);
    if (!installed) {
        if (argc!=1) return tool_error("hostname","persistent installed root required",0);
        return tool_write("hostname","fortress\n",9);
    }
    if (argc==2) {
        size_t n=tool_length(argv[1]);if (!n || n>63 || argv[1][0]=='-' || argv[1][n-1]=='-') return tool_error("hostname","invalid name",0);
        for (size_t i=0;i<n;i++) if (!((argv[1][i]>='a' && argv[1][i]<='z') || (argv[1][i]>='A' && argv[1][i]<='Z') ||
            (argv[1][i]>='0' && argv[1][i]<='9') || argv[1][i]=='-')) return tool_error("hostname","invalid name",0);
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/etc/hostname",VFS_O_WRONLY|VFS_O_CREAT|VFS_O_TRUNC|VFS_O_CLOEXEC,0644);
        if (fd<0) return tool_sys_error("hostname","cannot write name",0,fd);
        bool ok=tool_syscall(SYS_WRITE,fd,(uintptr_t)argv[1],n)==(long)n && tool_syscall(SYS_WRITE,fd,(uintptr_t)"\n",1)==1;
        long closed=tool_syscall(SYS_CLOSE,fd,0,0);
        return ok && !closed && !tool_syscall(SYS_SYNC,0,0,0) ? 0 : tool_error("hostname","name was not durably saved",0);
    }
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/etc/hostname",VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (fd==SYSCALL_ENOENT) return tool_write("hostname","fortress\n",9);
    if (fd<0) return tool_sys_error("hostname","cannot read name",0,fd);
    char text[65];long n=tool_syscall(SYS_READ,fd,(uintptr_t)text,sizeof(text));long closed=tool_syscall(SYS_CLOSE,fd,0,0);
    if (n<=0 || n>64 || closed || text[n-1]!='\n') return tool_error("hostname","invalid persistent name",0);
    for (long i=0;i<n-1;i++) if (!((text[i]>='a' && text[i]<='z') || (text[i]>='A' && text[i]<='Z') || (text[i]>='0' && text[i]<='9') || text[i]=='-')) return tool_error("hostname","invalid persistent name",0);
    return tool_write("hostname",text,n);
}
