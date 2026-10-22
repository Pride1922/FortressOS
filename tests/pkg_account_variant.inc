/* Same actual service and redo path, with distinct coherent bytes in all roles.
 * Root membership is explicit and the root shadow remains locked. */
int main(int argc,char **argv){
    integration_new[1]="root:x:0:root\n";
    integration_new[2]="root:*:::::::\n";
    return account_variant_previous_main(argc,argv);
}
