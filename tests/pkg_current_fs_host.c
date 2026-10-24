#define PKG_GEOMETRY_MAIN retained_geometry_main
#include "pkg_enroll_geometry_host.c"
#include "pkg_current_fs.inc"
int main(int argc,char **argv){
    assert(argc==7);char *args[]={argv[0],argv[1],"accept",argv[2],argv[3]};assert(!retained_geometry_main(5,args));
    size_t n;uint8_t *bytes=load(argv[1],&n);ext4_fault_disk_t d={.stable=bytes,.volatile_bytes=bytes,.bytes=n,.cut=-1};
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=denied_write,.flush=denied_flush,.priv=&d};
    gpt_policy_result_t gp;assert(gpt_parse_ex(&dev,&gp)&&gp==GPT_POLICY_PRIMARY_CONSISTENT);
    ext4_engine_t *e=NULL;int r=e4_open_journal_orphans_owned(&gpt_get_partition(0)->block_dev,true,&e,false,true);
    uint8_t *b=malloc(8192);assert(b);size_t length=0;
    for(unsigned i=0;i<2 && !r;i++){
        r=current_read_file(e->fs,i?"/usr/bin/demo":"/pkg/artifact.fpk",i?0755:0600,b,&length);
        if(!r){FILE *f=fopen(argv[4+i],"wbx");assert(f&&fwrite(b,1,length,f)==length&&!fclose(f));}
    }
    if(e)ext4_engine_close(e);
    assert(!d.writes&&!d.flushes&&!live&&!e4_engine_busy);free(b);free(bytes);
    assert(argv[6][0]=='1'?!r:r!=0);puts(r?"PASS native current metadata refusal":"PASS native current file readback writes=0 flushes=0");return 0;
}
