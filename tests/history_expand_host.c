#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "history_expand.h"
int main(void) {
    char out[64];
    assert(history_expand_sudo("sudo !!","id",out,sizeof(out))==1);
    assert(!strcmp(out,"sudo id"));
    assert(history_expand_sudo(" \tsudo\t!!  ","echo 'hello world'",out,sizeof(out))==1);
    assert(!strcmp(out,"sudo echo 'hello world'"));
    const char *literal[]={"","s","sudo","sudo ","sudo !","sudo !!!","sudo '!!'","sudo \"!!\"","echo sudo !!","sudo !! extra","sudo \\!\\!"};
    for (size_t i=0;i<sizeof(literal)/sizeof(literal[0]);i++)
        assert(history_expand_sudo(literal[i],"id",out,sizeof(out))==0);
    assert(history_expand_sudo("sudo !!",NULL,out,sizeof(out))==-1);
    assert(history_expand_sudo("sudo !!","",out,sizeof(out))==-1);
    assert(history_expand_sudo("sudo !!","id",out,7)==-2);
    assert(history_expand_sudo("sudo !!","id",out,8)==1);
    assert(!strcmp(out,"sudo id"));
    puts("PASS sudo history shorthand: previous command, whitespace, quoted literals, missing history and capacity");
    return 0;
}
