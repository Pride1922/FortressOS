#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <crypt.h>
#include "../user/tools/userdb.h"
static userdb_t db,old;
static const char passwd[]="root:x:0:0:Root:/root:/bin/shell\noperator:x:1000:1000:Operator:/run/user/1000:/bin/shell\n";
static const char groups[]="root:x:0:\nwheel:x:10:operator\nvideo:x:44:operator\ninput:x:104:operator\noperator:x:1000:\n";
static const char shadow[]="root:!:::::::\noperator::::::::\n";
int main(void) {
    assert(db_passwd(&db,passwd,strlen(passwd)) && db_group(&db,groups,strlen(groups)) && db_shadow(&db,shadow,strlen(shadow)) && db_validate(&db));
    uint32_t g[16];unsigned n;assert(db_groups(&db,db_user_name(&db,"operator"),g,&n) && n==4 && g[0]==1000 && g[1]==10 && g[2]==44 && g[3]==104);
    assert(db_user_id(&db,1000) && db_group_id(&db,44) && !db_user_id(&db,42));
    const char *badp[]={"u:x:0:0:/bin/shell\n","u:x:4294967296:0::/:/bin/shell\n","u:x:0:0::/a/../b:/bin/shell\n","u:x:0:0::/:/bin/shell:extra\n","u:x:0:0::/:/bin/shell\nu:x:1:0::/:/bin/shell\n","u:x:0:0::/:/bin/shell\nv:x:0:0::/:/bin/shell\n"};
    for (unsigned i=0;i<sizeof(badp)/sizeof(*badp);i++) {old=db;assert(!db_passwd(&db,badp[i],strlen(badp[i])) && !memcmp(&db,&old,sizeof(db)));}
    const char *bads[]={"u:!::::::\n","u:$6$bad:::::::\n","u:!:foo::::::\n","u:!::::::::\n","u:!:::::::\nu:!:::::::\n"};
    for (unsigned i=0;i<sizeof(bads)/sizeof(*bads);i++) {old=db;assert(!db_shadow(&db,bads[i],strlen(bads[i])) && !memcmp(&db,&old,sizeof(db)));}
    const char *badg[]={"g:x:0:a,\n","g:x:0:a,,b\n","g:x:0:a:extra\n","g:x:-1:\n","g:x:0:\nh:x:0:\n"};
    for (unsigned i=0;i<sizeof(badg)/sizeof(*badg);i++) {old=db;assert(!db_group(&db,badg[i],strlen(badg[i])) && !memcmp(&db,&old,sizeof(db)));}
    char large[DB_FILE_MAX+1];memset(large,'x',sizeof(large));old=db;
    assert(!db_passwd(&db,large,sizeof(large)) && !memcmp(&db,&old,sizeof(db)));
    char nul[sizeof(passwd)];memcpy(nul,passwd,sizeof(nul));nul[10]=0;assert(!db_passwd(&db,nul,sizeof(nul)-1));
    /* Deterministic hostile-byte coverage and complete failure rollback. */
    unsigned rng=23;
    for (unsigned kind=0;kind<3;kind++) for (unsigned i=0;i<10000;i++) {
        char text[DB_FILE_MAX];const char *seed=kind==0 ? passwd:kind==1 ? groups:shadow;
        size_t length=strlen(seed);memcpy(text,seed,length);
        rng=rng*1664525+1013904223;text[rng%length]=(char)(rng>>24);
        if (i%3==0) length=rng%(length+1);
        old=db;bool ok=kind==0 ? db_passwd(&db,text,length):kind==1 ? db_group(&db,text,length):db_shadow(&db,text,length);
        if (!ok) assert(!memcmp(&db,&old,sizeof(db)));
        db=old;
    }
    const char *keys[]={"", "Hello world!", "password", "a", "0123456789012345678901234567890123456789012345678901234567890123456789"};
    const char *settings[]={"$5$saltstring", "$5$rounds=1000$a", "$5$rounds=10000$abcdefghijklmnop", "$5$rounds=5000$./ABC123"};
    unsigned vectors=0;
    for (unsigned i=0;i<5;i++) for (unsigned j=0;j<4;j++) {
        char hash[96];const char *reference=crypt(keys[i],settings[j]);assert(reference && strlen(reference)<sizeof(hash));memcpy(hash,reference,strlen(reference)+1);
        assert(db_hash_valid(hash) && db_verify(keys[i],hash) && !db_verify("wrong",hash));vectors++;
        for (unsigned k=0;k<43;k+=7) {char wrong[96];memcpy(wrong,hash,strlen(hash)+1);size_t off=strlen(wrong)-43+k;wrong[off]=wrong[off]=='a' ? 'b' : 'a';assert(!db_verify(keys[i],wrong));}
    }
    assert(db_verify("Hello world!","$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5"));
    assert(!db_verify("p","!") && !db_verify("","*") && db_verify("","") && !db_verify("p",""));
    assert(!db_hash_valid("$5$rounds=100001$salt$aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    assert(db_shadow(&db,"u:!:0::::::\n",strlen("u:!:0::::::\n")) && db.shadows[0].restricted);
    printf("PASS bounded passwd/group/9-field shadow, duplicates/overflow/rollback, 30000 hostile inputs; %u glibc crypt vectors and fixed known vector\n",vectors);
}
