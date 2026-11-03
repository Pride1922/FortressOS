#include <assert.h>
#include <stdio.h>
#include "boot_diagnostics.h"
int main(void) {
 assert(!boot_selftests_requested(NULL,4096));
 const char *no[]={"","verbose","boot_selftests=0","xboot_selftests=1","boot_selftests=10","a=boot_selftests=1","boot_selftests=1x"};
 for(unsigned i=0;i<sizeof(no)/sizeof(no[0]);i++)assert(!boot_selftests_requested(no[i],4096));
 assert(boot_selftests_requested("boot_selftests=1",17));
 assert(boot_selftests_requested("verbose\tboot_selftests=1\nconsole=text",4096));
 for(size_t n=0;n<17;n++)assert(!boot_selftests_requested("boot_selftests=1",n));
 char unterminated[15]={'b','o','o','t','_','s','e','l','f','t','e','s','t','s','='};
 assert(!boot_selftests_requested(unterminated,sizeof unterminated));
 puts("PASS bounded exact boot self-test opt-in; verbose and near matches stay disabled");return 0;
}
