/* Actual CLI firmware preparation; only syscalls and decompressor are mocked.
 * The GUI launches this same `wifi init` flow, with no credential arguments. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static long fixture_call(long,uintptr_t,uintptr_t,uintptr_t);
#define WIFI_SYSCALL(n,a,b,c) fixture_call(n,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c))
#include "../user/wifi.c"
static unsigned state, opens, closes, loads;
static bool online, malformed;
static long fixture_call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_WIFI && a==WIFI_CMD_STATUS) {
        assert(c==sizeof(wifi_status_t));
        wifi_status_t *s=(void *)b; memset(s,0,sizeof(*s)); s->state=state; s->link_online=online;
        return 0;
    }
    if (nr==SYS_OPEN) {
        ++opens;
        return !strcmp((const char *)a,"/lib/firmware/iwlwifi-8265-36.ucode.gz") ? 3 : -2;
    }
    if (nr==SYS_CLOSE) { assert(a==3); ++closes; return 0; }
    if (nr==SYS_WIFI && a==WIFI_CMD_LOAD_FW) {
        assert(c==4 && !memcmp((void *)b,"FWOK",4)); ++loads; return 0;
    }
    if (nr==SYS_WRITE) return c;
    assert(!"Unexpected CLI preparation effect"); return -1;
}
int gzip_decode_callbacks(gzip_read_fn read,gzip_write_fn write,void *context) {
    (void)read; if (malformed) return -1;
    return write(context,"FWOK",4)==4 ? 0 : -1;
}
int main(void) {
    char *args[]={"wifi","init",NULL};
    state=WIFI_STATUS_FAILED;
    assert(!wifi_main(2,args) && opens==1 && closes==1 && loads==1);
    s_fw_loaded=false; state=WIFI_STATUS_SCANNING;
    assert(!wifi_main(2,args) && opens==2 && closes==2 && loads==2);
    s_fw_loaded=false; state=WIFI_STATUS_CONNECTED; online=true;
    assert(!wifi_main(2,args) && opens==2 && loads==2);
    s_fw_loaded=false; state=WIFI_STATUS_IDLE; online=false; malformed=true;
    assert(wifi_main(2,args)==1 && closes==3 && loads==2);
    puts("CLI/GUI firmware preparation PASS: FAILED/SCANNING do not skip loading; online reuse; decoder failure; exact descriptor cleanup");
}
