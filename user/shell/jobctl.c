#include "jobctl.h"
#include "jobs.h"
#include "pipeline.h"
#include "builtins.h"
#include "io.h"

static int error(const char *name, const char *message) {
    puts_err(name); puts_err(": "); puts_err(message); puts_err("\n");
    return 1;
}
static int signal_number(const char *s) {
    static const struct { const char *name; unsigned value; } names[] = {
        {"HUP",SIGHUP}, {"INT",SIGINT}, {"KILL",SIGKILL}, {"PIPE",SIGPIPE},
        {"TERM",SIGTERM}, {"CHLD",SIGCHLD}, {"CONT",SIGCONT}, {"STOP",SIGSTOP},
        {"TSTP",SIGTSTP}, {"TTIN",SIGTTIN}, {"TTOU",SIGTTOU}
    };
    if (s[0]=='S' && s[1]=='I' && s[2]=='G') s+=3;
    for (unsigned i=0; i<sizeof(names)/sizeof(names[0]); ++i)
        if (equal(s,names[i].name)) return (int)names[i].value;
    if (!*s) return -1;
    unsigned n=0;
    for (;*s;++s) {
        if (*s<'0' || *s>'9' || n>(31u-(unsigned)(*s-'0'))/10) return -1;
        n=n*10+(unsigned)(*s-'0');
    }
    return !n || (SIGNAL_SUPPORTED & SIGNAL_BIT(n)) ? (int)n : -1;
}
int jobctl_exec(int argc, const char *const *argv) {
    if (argc<1 || !argv || !argv[0]) return 1;
    enum builtin command=builtin_find(argv[0]);
    if (command==CMD_JOBS) {
        if (argc!=1) return error("jobs","usage: jobs");
        jobs_reap_children();
        for (int i=0; i<jobs_max(); ++i) {
            const job_t *job=jobs_get(i);
            if (job->in_use && !job->notified) jobs_print_state(i,true);
        }
        jobs_gc();
        return 0;
    }
    if (command!=CMD_FG && command!=CMD_BG && command!=CMD_KILL) return 1;
    if ((command==CMD_KILL && (argc<2 || argc>3)) ||
        (command!=CMD_KILL && argc>2)) return error(argv[0],"invalid arguments");
    int sig=command==CMD_KILL ? (argc==3 ? signal_number(argv[2]) : SIGTERM) : SIGCONT;
    if (sig<0) return error(argv[0],"unsupported signal");
    jobs_reap_children(); jobs_gc();
    int slot=jobs_resolve(argc>1 ? argv[1] : NULL);
    if (slot<0) return error(argv[0],slot==-2 ? "invalid job specifier" : "no such job");
    const job_t *job=jobs_get(slot);
    if (command==CMD_FG) return pipeline_foreground(slot,false,0);
    long result=call(SYS_KILL,(uintptr_t)-job->pgid,(uintptr_t)sig,0);
    if (result<0) return error(argv[0],result==SYSCALL_EACCES || result==SYSCALL_EPERM ? "permission denied" : result==SYSCALL_ESRCH ? "job no longer exists" : "signal failed");
    if (sig==SIGCONT) jobs_mark_running(slot);
    if (command==CMD_BG) {
        jobs_set_foreground(slot,false);
        jobs_print_state(slot,true);
    }
    return 0;
}
