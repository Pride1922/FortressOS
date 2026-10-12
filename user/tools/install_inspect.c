#include "common.h"
#include "install_abi.h"
static void say(const char *s) {(void)tool_write("inspect",s,tool_length(s));}
static void number(uint64_t value) {char text[32];tool_format_u64(text,value);say(text);}
static void field(const char *text,size_t length) {
    while (length && (text[length-1]==' ' || !text[length-1])) length--;
    char copy[65];if (length>64) length=64;
    for (size_t i=0;i<length;i++) copy[i]=text[i]>=32 && text[i]<=126 ? text[i] : '?';
    copy[length]=0;say(copy);
}
int install_main(int argc,char **argv) {
    (void)argc;(void)argv;
    say("\nFortressOS NVMe hardware inspection\nInstallation is disabled in this image.\nActual hardware identity; no synthetic target.\n\n");
    uint32_t index=0;install_identity_v1_t id={0};
    long result=SYS_INSTALLCTL;register uintptr_t a4 __asm__("r10")=(uintptr_t)&id;
    register uintptr_t a5 __asm__("r8")=sizeof(id);
    __asm__ volatile("syscall":"+a"(result):"D"((uintptr_t)INSTALL_ENUM),"S"((uintptr_t)&index),"d"((uintptr_t)sizeof(index)),"r"(a4),"r"(a5):"rcx","r11","cc","memory");
    if (result!=1 || id.flags) {say("Inspection unavailable: requires one supported 512-byte-sector NVMe namespace.\n");}
    else {
        say("Actual model: ");field(id.model,40);say("\nActual serial: ");field(id.serial,20);
        say("\nCapacity MiB: ");number(id.sector_count/2048);say("\nSector count: ");number(id.sector_count);
        say("\nLogical sector bytes: ");number(id.sector_size);say("\nPCI controller: ");number(id.segment);say(":");number(id.bus);say(":");number(id.device);say(".");number(id.function);
        say("\nNamespace: ");number(id.nsid);say("\nIdentity digest: ");
        char hex[65];const char *digits="0123456789abcdef";
        for (unsigned i=0;i<32;i++) {hex[i*2]=digits[id.digest[i]>>4];hex[i*2+1]=digits[id.digest[i]&15];}hex[64]=0;say(hex);say("\n");
    }
    say("\nNo installation or disk-sector operations are available.\nPhotograph this screen, then power off and remove this USB.\n");
    for (;;) {unsigned char key;(void)tool_syscall(SYS_INPUT_READ,(uintptr_t)&key,1,(uintptr_t)-1);}
}
