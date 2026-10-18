#ifndef FORTRESS_KILL_CLI_H
#define FORTRESS_KILL_CLI_H
#include "types.h"
#include "signal_abi.h"

static inline bool kill_equal(const char *a,const char *b) {
    while (*a && *a==*b) {a++;b++;}
    return *a==*b;
}
static inline int kill_signal(const char *s) {
    static const struct {const char *name;unsigned value;} names[]={
        {"HUP",SIGHUP},{"INT",SIGINT},{"KILL",SIGKILL},{"PIPE",SIGPIPE},
        {"TERM",SIGTERM},{"CHLD",SIGCHLD},{"CONT",SIGCONT},{"STOP",SIGSTOP},
        {"TSTP",SIGTSTP},{"TTIN",SIGTTIN},{"TTOU",SIGTTOU}
    };
    if (s[0]=='S' && s[1]=='I' && s[2]=='G') s+=3;
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
        if (kill_equal(s,names[i].name)) return (int)names[i].value;
    if (!*s) return -1;
    unsigned n=0;
    for (;*s;s++) {
        if (*s<'0' || *s>'9' || n>(31u-(unsigned)(*s-'0'))/10) return -1;
        n=n*10+(unsigned)(*s-'0');
    }
    return !n || (SIGNAL_SUPPORTED & SIGNAL_BIT(n)) ? (int)n : -1;
}
/* One explicit positive PID. Process-group/all-process targets are not exposed. */
static inline bool kill_pid(const char *s,int64_t *out) {
    uint64_t n=0;
    if (!*s) return false;
    for (;*s;s++) {
        if (*s<'0' || *s>'9' || n>((uint64_t)INT64_MAX-(unsigned)(*s-'0'))/10) return false;
        n=n*10+(unsigned)(*s-'0');
    }
    if (!n) return false;
    *out=(int64_t)n;return true;
}
static inline bool kill_arguments(int argc,const char *const *argv,const char **target,int *sig) {
    *sig=SIGTERM;
    if (argc==4 && kill_equal(argv[1],"-s")) {
        *sig=kill_signal(argv[2]);*target=argv[3];
    } else if (argc==3 && argv[1][0]=='-') {
        *sig=kill_signal(argv[1]+1);*target=argv[2];
    } else if (argc==2 || argc==3) {
        *target=argv[1];
        if (argc==3) *sig=kill_signal(argv[2]);
    } else return false;
    return *sig>=0 && **target;
}
#endif
