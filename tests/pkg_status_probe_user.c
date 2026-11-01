/* Disposable Ring 3 probe, absent from normal media. Separate writer process
 * exercises ordinary VFS activity while the parent acquires/releases sessions. */
#include "common.h"
#include "pkg_abi.h"
static char line[]="history activity during pkg read\n";
static long control(unsigned operation,uint64_t session,pkg_result_v1 *result) {
    pkg_request_v1 request={.size=sizeof request,.version=1,.operation=operation,.session=session};
    return tool_syscall(SYS_PKGCTL,(uintptr_t)&request,(uintptr_t)result,sizeof request);
}
int pkg_status_probe_main(int argc,char **argv) {
    if(argc==2&&tool_equal(argv[1],"--writer")) {
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/home/operator/pkg-status-history",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_APPEND,0600);
        if(fd<0)return 1;
        for(unsigned i=0;i<160;i++)if(tool_syscall(SYS_WRITE,fd,(uintptr_t)line,sizeof line-1)!=sizeof line-1)return 1;
        return tool_syscall(SYS_CLOSE,fd,0,0)<0?1:0;
    }
    char *args[]={"/bin/pkg-status-probe","--writer",NULL};
    long child=tool_syscall(SYS_SPAWN,(uintptr_t)args[0],(uintptr_t)args,0);
    if(child<0)return 1;
    timespec_t delay={.tv_nsec=1000000};
    unsigned failures=0;long first=0;
    for(unsigned i=0;i<600;i++) {
        pkg_result_v1 result={0};long r=control(PKG_ACQUIRE,0,&result);
        if(!r) {
            uint64_t session=result.session;
            if(result.size!=sizeof result||result.version!=1||!session||result.phase!=PKG_PHASE_IDLE)r=SYSCALL_EIO;
            long released=control(PKG_RELEASE,session,&result);if(!r)r=released;
        }
        if(r){failures++;if(!first)first=r;}
        if(!(i%10))(void)tool_syscall(SYS_NANOSLEEP,(uintptr_t)&delay,0,0);
    }
    int64_t status=0;
    if(tool_syscall(SYS_WAIT,child,(uintptr_t)&status,0)<0||status)return 1;
    char message[112]="PKG_STATUS_STRESS failures=";
    size_t n=tool_length(message);n+=tool_format_u64(message+n,failures);
    const char *middle=" first=";for(unsigned i=0;middle[i];i++)message[n++]=middle[i];
    n+=tool_format_i64(message+n,first);message[n++]='\n';
    if(tool_write("pkg-status-probe",message,n))return 1;
    return failures?1:0;
}
