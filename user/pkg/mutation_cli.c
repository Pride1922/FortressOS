#include "mutation_cli.h"
#include "../tools/common.h"
int pkg_mutation_cli(int argc,char **argv,const fpk_catalog_policy *policy,
                     const pkg_mutation_backend *backend) {
    tool_output_failed=false;
    if(argc<1||argc>5||!argv)return tool_error("pkg","invalid arguments",NULL);
    for(int i=0;i<argc;i++) {
        if(!argv[i])return tool_error("pkg","invalid arguments",NULL);
        size_t n=0;while(n<256&&argv[i][n])n++;
        if(n==256)return tool_error("pkg","argument too long",NULL);
    }
    if(argc==2&&tool_equal(argv[1],"--help")&&backend&&backend->mutate){
        static const char help[]="pkg status\npkg recover\npkg install PACKAGE\npkg remove PACKAGE\n"
#ifdef FORTRESS_PKG_FETCH
            "pkg fetch PACKAGE NEW-ABSOLUTE-DIRECTORY\n"
            "pkg install PACKAGE ABSOLUTE-STAGING-DIRECTORY\n"
#endif
            "pkg inspect ARTIFACT BYTES SHA256\npkg verify CATALOG PACKAGE ARTIFACT\n";
        return tool_write("pkg",help,sizeof help-1);
    }
    bool install=argc>1&&tool_equal(argv[1],"install");
    bool remove=argc>1&&tool_equal(argv[1],"remove");
    if(!install&&!remove)return pkg_transaction_cli(argc,argv,policy,backend?&backend->transactions:NULL);
    if(argc!=3)return tool_error("pkg","install/remove require one package name",NULL);
    char name[33];size_t n=tool_length(argv[2]);
    if(!fpk_identity_valid((const uint8_t *)argv[2],n))return tool_error("pkg","invalid package name",NULL);
    for(size_t i=0;i<n;i++)name[i]=argv[2][i];
    name[n]=0;
    if(!backend||!backend->mutate)return tool_error("pkg","mutation backend unavailable",NULL);
    if(backend->mutate(backend->transactions.context,remove,name,n))
        return tool_error("pkg","operation not confirmed; inspect status before retrying",NULL);
    static const char done[]="operation complete\n";
    return tool_write("pkg",done,sizeof done-1);
}
