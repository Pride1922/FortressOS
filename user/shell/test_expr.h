#ifndef SHELL_TEST_EXPR_H
#define SHELL_TEST_EXPR_H
#include "io.h"
static inline bool test_integer(const char *text,int64_t *value) {
    bool negative=false;
    if(*text=='-' || *text=='+') negative=*text++=='-';
    if(!*text) return false;
    uint64_t n=0,limit=negative ? (1ULL<<63) : ((1ULL<<63)-1);
    while(*text) {
        if(*text<'0' || *text>'9') return false;
        unsigned digit=(unsigned)(*text++-'0');
        if(n>(limit-digit)/10) return false;
        n=n*10+digit;
    }
    *value=negative ? (n==(1ULL<<63) ? (-9223372036854775807LL-1) : -(int64_t)n) : (int64_t)n;
    return true;
}
/* Fixed arity grammar; no -a/-o expression parser or implicit truncation. */
static inline int shell_test(int argc,const char *const *argv) {
    bool bracket=equal(argv[0],"[");int n=argc-1;const char *const *a=argv+1;
    if(bracket) {if(!n || !equal(a[n-1],"]")) return 2;n--;}
    bool negate=n>1 && equal(a[0],"!");if(negate) {a++;n--;}
    bool yes=false;
    if(!n) yes=false;
    else if(n==1) yes=a[0][0]!=0;
    else if(n==2) {
        if(equal(a[0],"-n")) yes=a[1][0]!=0;
        else if(equal(a[0],"-z")) yes=!a[1][0];
        else if(equal(a[0],"-e") || equal(a[0],"-f") || equal(a[0],"-d")) {
            vfs_stat_t st;long r=call(SYS_STAT,(uintptr_t)a[1],(uintptr_t)&st,0);
            if(r && r!=SYSCALL_ENOENT && r!=SYSCALL_ENOTDIR) return 2;
            yes=!r && (equal(a[0],"-e") || (equal(a[0],"-f") ? st.type==VFS_FILE : st.type==VFS_DIRECTORY));
        } else return 2;
    } else if(n==3) {
        if(equal(a[1],"=")) yes=equal(a[0],a[2]);
        else if(equal(a[1],"!=")) yes=!equal(a[0],a[2]);
        else {
            int64_t x,y;if(!test_integer(a[0],&x) || !test_integer(a[2],&y)) return 2;
            if(equal(a[1],"-eq")) yes=x==y;
            else if(equal(a[1],"-ne")) yes=x!=y;
            else if(equal(a[1],"-lt")) yes=x<y;
            else if(equal(a[1],"-le")) yes=x<=y;
            else if(equal(a[1],"-gt")) yes=x>y;
            else if(equal(a[1],"-ge")) yes=x>=y;
            else return 2;
        }
    } else return 2;
    return (yes!=negate) ? 0 : 1;
}
#endif
