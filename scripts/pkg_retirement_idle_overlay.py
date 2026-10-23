"""Exact copied-source completion hooks; no production caller."""
from pkg_retirement_overlay import apply_retirement
from test_pkg_service_integration import function

def apply_idle(work,replace,generated):
 apply_retirement(work,replace,generated)
 p='tests/pkg_retirement_integrated.inc'
 replace(p,'static uint8_t xr_locations[512];','static uint8_t xr_locations[512];\nstatic uint8_t xi_locator[128],xi_pin[32];\nstatic unsigned xi_stage;\nstatic int xi_gate(struct ext4_engine *,bool);\nstatic int xi_commit(struct ext4_engine *);\nstatic int xi_activate(unsigned,unsigned);\nstatic int xi_retire(unsigned);\nstatic void xi_export(struct ext4_mount *,const char *);\nstatic int xi_fence(struct ext4_engine *);')
 replace(p,'assert(n==1568);','assert(n==1728);memcpy(xi_pin,p+1568,32);memcpy(xi_locator,p+1600,128);')
 replace(p,'static uint8_t zero[PE_MAX];','static uint8_t zero[PE_MAX];\n ad_write("/mnt/pkg/completion.cert",zero,1728);')
 replace(p,'static int xr_gate(ext4_engine_t *e,bool planned){','static int xi_archive_gate(ext4_engine_t *e,bool planned){')
 replace(p,'static int e4_semantic_commit(struct ext4_engine *e){','static int xi_archive_commit(struct ext4_engine *e){')
 replace(p,' save(prefix,"locations",xr_locations,sizeof xr_locations);',' save(prefix,"locations",xr_locations,sizeof xr_locations);xi_export(fs,prefix);')
 replace(p,'bool ok=rn==1568&&n<=PE_MAX;','bool ok=rn==1728&&n<=PE_MAX;')
 replace(p,'int main(int argc,char **argv){','int xi_previous_main(int argc,char **argv){')
 replace(p,'assert(argc==7||argc==10);','assert(argc==7||argc==11);')
 replace(p,'assert(!activate||argc==10);','assert(!activate||argc==11);')
 replace(p,'  r=e4_namespace_end(sv_fs->engine,r);xr_typed=xr_bootstrap=false;', '  size_t end_n;uint8_t *end=load(argv[10],&end_n);assert(end_n==1728);\n  if(!r)r=xr_stage(sv_fs->engine,"/mnt/pkg/completion.cert",end,end_n);\n  free(end);\n  r=e4_namespace_end(sv_fs->engine,r);xr_typed=xr_bootstrap=false;')
 replace(p,' (void)e;return ino==xr_result.binding.inode&&parent==e4_u32(ad_locator(3)+4)?0:-VFS_EROFS;',' (void)e;return xi_activate(ino,parent);')
 replace(p,' (void)e;return ino==xr_result.binding.inode?0:-VFS_EROFS;',' (void)e;return xi_retire(ino);')
 replace(p,'static bool xr_name_allowed(const char *name){','static bool xr_name_allowed(const char *name){if(!strcmp(name,"completion.cert"))return true;')
 replace('tests/pkg_account_durable.inc','assert(n<=PE_MAX&&rn==1568);','assert(n<=PE_MAX&&rn==1728);')
 replace('tests/pkg_account_service_durable.inc','if(xr_active)return -VFS_EAGAIN;','if(xr_active&&xi_stage!=11)return -VFS_EAGAIN;')
 replace('tests/pkg_mount_admission.inc','if(xr_active){for(unsigned i=436;', 'if(xr_active&&xi_stage>=11)return 0;\n if(xr_active){for(unsigned i=436;')
 replace('tests/pkg_mount_admission.inc','if(xr_active)return xr_fence(e);','if(xr_active)return xi_fence(e);')
 copied=work/'tests/pkg_retirement_integrated.inc'
 text=copied.read_text();text=text.replace(function(text,'static int xr_fence('),'');copied.write_text(text)
 early='static unsigned xi_stage;\nstatic int xi_fence(struct ext4_engine *);\n'
 generated.write_text(generated.read_text().replace('static int xr_fence(struct ext4_engine *);',early)+'\n#include "pkg_retirement_idle.inc"\n')
