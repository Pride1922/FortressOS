/* Compatibility entry; exercise the normal system frontend. */
#include "../user/pkg/system_cli.c"
int pkg_private_cli(int argc,char **argv){return pkg_system_main(argc,argv);}
