#include "common.h"
#include "digest.h"

#define MANIFEST_LINE 512
static char manifest_line[MANIFEST_LINE + 1];
static uint8_t manifest_buffer[TOOL_BUFFER_SIZE];
static const char *name;
static digest_kind_t kind;
static bool safe_label(const char *s) {
    size_t n=0;
    for (; s[n]; n++) if (n >= VFS_MAX_PATH-1 || s[n]=='\n' || s[n]=='\r' || s[n]=='\\') return false;
    return n != 0;
}
static int hash_fd(int fd, uint8_t result[32]) {
    digest_t ctx;
    digest_init(&ctx, kind);
    for (;;) {
        long n=tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0 || (size_t)n > TOOL_BUFFER_SIZE) return tool_error(name,"read error",NULL);
        if (!n) break;
        if (!digest_update(&ctx,tool_buffer,(size_t)n)) return tool_error(name,"input too large",NULL);
    }
    digest_final(&ctx,result);
    return 0;
}
static int hash_path(const char *path, uint8_t result[32], bool allow_stdin) {
    if (!safe_label(path)) return tool_error(name,"unsupported filename",path);
    bool stdin_file=tool_equal(path,"-");
    if (stdin_file && !allow_stdin) return tool_error(name,"stdin entry not supported in manifest",path);
    long fd=0;
    if (!stdin_file) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT,(uintptr_t)path,(uintptr_t)&st,0)<0 || st.type!=VFS_FILE)
            return tool_error(name,"not a readable regular file",path);
        fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
        if (fd<0) return tool_error(name,"cannot open",path);
    }
    int status=hash_fd((int)fd,result);
    if (!stdin_file && tool_syscall(SYS_CLOSE,fd,0,0)<0) status=tool_error(name,"close error",path);
    return status;
}
static int hex_value(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static int verify_line(size_t len) {
    size_t bytes=digest_size(kind), chars=bytes*2;
    if (len && manifest_line[len-1]=='\r') manifest_line[--len]=0;
    if (len<=chars+2 || manifest_line[chars]!=' ' ||
        (manifest_line[chars+1]!=' ' && manifest_line[chars+1]!='*'))
        return tool_error(name,"malformed manifest entry",NULL);
    uint8_t expected[32], actual[32];
    for (size_t i=0; i<bytes; i++) {
        int hi=hex_value(manifest_line[i*2]), lo=hex_value(manifest_line[i*2+1]);
        if (hi<0 || lo<0) return tool_error(name,"malformed digest",NULL);
        expected[i]=(uint8_t)(hi*16+lo);
    }
    const char *path=manifest_line+chars+2;
    int status=hash_path(path,actual,false);
    if (!status) for (size_t i=0; i<bytes; i++) if (actual[i]!=expected[i]) { status=1; break; }
    if (tool_write(name,path,tool_length(path))) return 1;
    const char *suffix=status ? ": FAILED\n" : ": OK\n";
    if (tool_write(name,suffix,tool_length(suffix))) return 1;
    return status;
}
static int verify(const char *path) {
    if (!safe_label(path)) return tool_error(name,"unsupported filename",path);
    bool owned=!tool_equal(path,"-");
    long fd=owned ? tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_CLOEXEC,0) : 0;
    if (fd<0) return tool_error(name,"cannot open manifest",path);
    size_t len=0, entries=0;
    size_t buffered=0, position=0;
    bool bad=false;
    int status=0;
    for (;;) {
        /* Separate fixed buffers preserve unread manifest bytes while hash_fd
         * streams the referenced file. Never allocate from the file length. */
        if (position==buffered) {
            long got=tool_syscall(SYS_READ,fd,(uintptr_t)manifest_buffer,sizeof(manifest_buffer));
            if (got<0 || (size_t)got>sizeof(manifest_buffer)) { status=tool_error(name,"manifest read error",path); break; }
            buffered=(size_t)got; position=0;
        }
        long n=position<buffered ? 1:0;
        uint8_t c=n ? manifest_buffer[position++]:0;
        if (!n || c=='\n') {
            if (len || bad || n) {
                entries++;
                manifest_line[len]=0;
                if (bad) status=tool_error(name,"overlong or invalid manifest entry",NULL);
                else if (verify_line(len)) status=1;
            }
            len=0; bad=false;
            if (!n || tool_output_failed) break;
        } else if (!c || len==MANIFEST_LINE) bad=true;
        else if (!bad) manifest_line[len++]=(char)c;
    }
    if (!entries) status=tool_error(name,"empty manifest",path);
    if (owned && tool_syscall(SYS_CLOSE,fd,0,0)<0) status=tool_error(name,"close error",path);
    return status;
}
static int checksum_main(int argc, char **argv, digest_kind_t algorithm) {
    kind=algorithm; name=kind==DIGEST_MD5 ? "md5sum" : "sha256sum";
    tool_output_failed=false;
    if (argc==2 && tool_equal(argv[1],"--help")) {
        const char *help=" [--] [FILE ...]\n       -c MANIFEST\nNo files or - hashes stdin. Fixed-memory binary streaming.\nManifest filenames are relative to cwd; escaped names and stdin entries are unsupported.\nMD5 is for compatibility; use SHA-256 for downloads.\n";
        return tool_write(name,"Usage: ",7) || tool_write(name,name,tool_length(name)) || tool_write(name,help,tool_length(help));
    }
    if (argc>1 && tool_equal(argv[1],"-c")) {
        if (argc!=3) { tool_error(name,"use -c MANIFEST",NULL); return 2; }
        return verify(argv[2]);
    }
    int first=1;
    if (argc>1 && tool_equal(argv[1],"--")) first++;
    else if (argc>1 && argv[1][0]=='-' && argv[1][1]) {
        tool_error(name,"invalid option; use --help",NULL); return 2;
    }
    int status=0, end=argc==first ? first+1 : argc;
    for (int i=first; i<end; i++) {
        const char *path=i<argc ? argv[i] : "-";
        uint8_t result[32]; char hex[64];
        if (hash_path(path,result,true)) { status=1; continue; }
        static const char digits[]="0123456789abcdef";
        size_t size=digest_size(kind);
        for (size_t j=0; j<size; j++) { hex[j*2]=digits[result[j]>>4]; hex[j*2+1]=digits[result[j]&15]; }
        if (tool_write(name,hex,size*2) || tool_write(name,"  ",2) ||
            tool_write(name,path,tool_length(path)) || tool_write(name,"\n",1)) { status=1; break; }
    }
    return status;
}
int md5sum_main(int argc, char **argv) { return checksum_main(argc,argv,DIGEST_MD5); }
int sha256sum_main(int argc, char **argv) { return checksum_main(argc,argv,DIGEST_SHA256); }
