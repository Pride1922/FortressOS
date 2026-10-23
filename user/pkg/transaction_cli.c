#include "transaction_cli.h"
#include "../tools/common.h"
static int valid(const pkg_transaction_status *s) {
    if(s->phase>PKG_TX_RETIRING || !s->sequence || !s->account_sequence || !s->credential_generation) return 0;
    for(unsigned i=0;i<2;i++) {
        if(s->remaining[i]>UINT64_MAX-s->charged[i]) return 0;
        if(s->phase==PKG_TX_IDLE && (s->remaining[i] || s->charged[i])) return 0;
    }
    return 1;
}
static int number(const char *label,uint64_t n) {
    char text[32]; size_t length=tool_format_u64(text,n);
    return tool_write("pkg",label,tool_length(label)) || tool_write("pkg",text,length);
}
static int report(const pkg_transaction_status *s,int recovered) {
    static const char *const names[]={"IDLE","RESERVED","PENDING","RETIRING"};
    const char *prefix=recovered?"recovery complete: ":"state: ";
    if(tool_write("pkg",prefix,tool_length(prefix)) ||
       tool_write("pkg",names[s->phase],tool_length(names[s->phase])) ||
       number("\nsequence: ",s->sequence) || number("\naccount sequence: ",s->account_sequence) ||
       number("\ncredential generation: ",s->credential_generation) ||
       number("\nremaining blocks: ",s->remaining[0]) || number(" inodes: ",s->remaining[1]) ||
       number("\ncharged blocks: ",s->charged[0]) || number(" inodes: ",s->charged[1])) return 1;
    return tool_write("pkg","\n",1);
}
int pkg_transaction_cli(int argc,char **argv,const fpk_catalog_policy *policy,
                        const pkg_transaction_backend *backend) {
    tool_output_failed=false;
    if(argc<1 || argc>5 || !argv) return tool_error("pkg","usage: pkg --help",NULL);
    for(int i=0;i<argc;i++) {
        if(!argv[i]) return tool_error("pkg","invalid arguments",NULL);
        size_t n=0; while(n<256 && argv[i][n]) n++;
        if(n==256) return tool_error("pkg","argument too long",NULL);
    }
    if(argc==2 && tool_equal(argv[1],"--help") && backend) {
        static const char help[]="pkg status\npkg recover\npkg inspect ARTIFACT BYTES SHA256\npkg verify CATALOG PACKAGE ARTIFACT\nInstallation is unavailable.\n";
        return tool_write("pkg",help,sizeof(help)-1);
    }
    int status=argc>=2 && tool_equal(argv[1],"status");
    int recover=argc>=2 && tool_equal(argv[1],"recover");
    if(!status && !recover) return pkg_cli(argc,argv,policy);
    if(argc!=2) return tool_error("pkg","status/recover take no arguments",NULL);
    if(!backend || (status?!backend->status:!backend->recover))
        return tool_error("pkg","transaction backend unavailable",NULL);
    pkg_transaction_status snapshot={0};
    int r=status?backend->status(backend->context,&snapshot):backend->recover(backend->context,&snapshot);
    if(r) return tool_error("pkg",status?"status unavailable":"recovery not confirmed; inspect status before retrying",NULL);
    if(!valid(&snapshot) || (recover && snapshot.phase!=PKG_TX_IDLE))
        return tool_error("pkg","transaction result rejected",NULL);
    return report(&snapshot,recover);
}
