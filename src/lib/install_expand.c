#include "install_payload.h"
#include "string.h"

/* A new installation only: preserve the verified seed's existing inodes/data,
 * consume its deliberately free descriptor-growth padding, initialize new
 * groups, then publish checksummed superblocks. Readiness remains invalid. */
static uint32_t u32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t u16(const uint8_t *p) {return p[0]|(uint16_t)p[1]<<8;}
static void p32(uint8_t *p,uint32_t n) {for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i));}
static void p16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static uint32_t crc(uint32_t c,const void *bytes,size_t n) {
    const uint8_t *p=bytes;while (n--) {c^=*p++;for (unsigned i=0;i<8;i++) c=(c>>1)^(0x82f63b78u&(0u-(c&1)));}return c;
}
static bool power(uint32_t g,unsigned base) {while (g>1 && !(g%base)) g/=base;return g==1;}
static bool backup(uint32_t g) {return !g || g==1 || power(g,3) || power(g,5) || power(g,7);}
static void bit(uint8_t *p,uint32_t n) {p[n/8]|=(uint8_t)(1u<<(n%8));}
static int read_block(const install_target_t *t,uint32_t block,uint8_t *out) {
    int r=t->read(t->context,INSTALL_ROOT_LBA+(uint64_t)block*8,8,out);return r>0 ? -5 : r;
}
static int write_block(const install_target_t *t,uint32_t block,const uint8_t *bytes,install_expand_work_t *w) {
    uint64_t l=INSTALL_ROOT_LBA+(uint64_t)block*8;
    if (l>=t->sectors || 8>t->sectors-l) return -22;
    int r=t->write(t->context,l,8,bytes);if (r) return r<0 ? r : -5;
    r=read_block(t,block,w->verify);return r ? r : memcmp(bytes,w->verify,4096) ? -5 : 0;
}
static void seal(uint8_t *d,uint32_t g,uint32_t seed) {
    uint8_t number[4];p32(number,g);p16(d+30,0);p16(d+30,(uint16_t)crc(crc(seed,number,4),d,32));
}
int install_root_expand(const install_target_t *t,uint64_t sectors,install_expand_work_t *w) {
    if (!t || !w || !t->read || !t->write || !t->flush || t->sector_size!=512 ||
        sectors<INSTALL_ROOT_BYTES/512 || t->sectors<INSTALL_ROOT_LBA+33 || sectors>t->sectors-INSTALL_ROOT_LBA-33) return -22;
    uint64_t count=sectors/8;if (count>INSTALL_ROOT_MAX_BLOCKS) return -27;
    uint32_t blocks=(uint32_t)count,groups=(blocks+32767)/32768,gdt=(groups+127)/128;
    /* A tail too small for its metadata stays outside the filesystem. */
    if (blocks%32768 && blocks%32768<(backup(groups-1) ? 1+gdt : 0)+10) {
        blocks-=blocks%32768;groups--;gdt=(groups+127)/128;
    }
    int r=read_block(t,0,w->block);if (r) return r;memcpy(w->super,w->block+1024,1024);
    uint8_t *sb=w->super;
    if (u16(sb+56)!=0xef53 || u32(sb+4)!=65536 || u32(sb)!=256 || u32(sb+24)!=2 ||
        u32(sb+20) || u32(sb+32)!=32768 || u32(sb+40)!=128 || u16(sb+88)!=256 ||
        u32(sb+92)!=4 || u32(sb+96)!=0x42 || u32(sb+100)!=0x403 || u16(sb+206) ||
        u16(sb+58)!=1 || u32(sb+232) || u32(sb+1020)!=crc(UINT32_MAX,sb,1020)) return -22;
    uint32_t seed=crc(UINT32_MAX,sb+104,16),free_blocks=0,free_inodes=0;
    memset(w->descriptors,0,sizeof(w->descriptors));
    r=read_block(t,1,w->block);if (r) return r;memcpy(w->descriptors,w->block,64);
    /* Validate seed padding and bitmaps before the first expansion write. */
    for (uint32_t g=0;g<2;g++) {
        uint8_t *d=w->descriptors+g*32,copy[32];memcpy(copy,d,32);uint16_t stored=u16(d+30);seal(copy,g,seed);
        uint32_t start=g*32768;
        if (u16(copy+30)!=stored || u32(d)!=start+65 || u32(d+4)!=start+66 || u32(d+8)!=start+67) return -22;
        r=read_block(t,u32(d),w->block);if (r) return r;
        if ((uint16_t)crc(seed,w->block,4096)!=u16(d+24)) return -5;
        for (uint32_t b=2;b<=64;b++) if (w->block[b/8]&(1u<<(b%8))) return -22;
    }
    for (uint32_t g=0;g<groups;g++) {
        uint32_t start=g*32768,n=blocks-start;if (n>32768) n=32768;
        uint8_t *d=w->descriptors+g*32;uint32_t prefix=backup(g) ? 1+gdt : 0;
        if (g<2) {
            r=read_block(t,u32(d),w->block);if (r) return r;
            for (uint32_t b=2;b<=gdt;b++) bit(w->block,b);
            p16(d+12,(uint16_t)(u16(d+12)-(gdt-1)));
        } else {
            p32(d,start+prefix);p32(d+4,start+prefix+1);p32(d+8,start+prefix+2);
            p16(d+12,(uint16_t)(n-prefix-10));p16(d+14,128);p16(d+18,4);p16(d+28,128);
            memset(w->block,0,4096);for (uint32_t b=0;b<prefix+10;b++) bit(w->block,b);
            for (uint32_t b=n;b<32768;b++) bit(w->block,b);
        }
        p16(d+24,(uint16_t)crc(seed,w->block,4096));
        r=write_block(t,u32(d),w->block,w);if (r) return r;
        if (g>=2) {
            memset(w->block,0xff,4096);memset(w->block,0,16);p16(d+26,(uint16_t)crc(seed,w->block,16));
            r=write_block(t,u32(d+4),w->block,w);if (r) return r;
            memset(w->block,0,4096);
            for (unsigned i=0;i<8;i++) {r=write_block(t,u32(d+8)+i,w->block,w);if (r) return r;}
        }
        free_blocks+=u16(d+12);free_inodes+=u16(d+14);seal(d,g,seed);
    }
    p32(sb,groups*128);p32(sb+4,blocks);p32(sb+8,0);p32(sb+12,free_blocks);p32(sb+16,free_inodes);
    /* Cached overhead is optional; zero asks readers to derive it. */
    p32(sb+584,0);
    for (uint32_t g=0;g<groups;g++) if (backup(g)) {
        for (uint32_t i=0;i<gdt;i++) {
            r=write_block(t,g*32768+1+i,w->descriptors+i*4096,w);if (r) return r;
        }
        if (g) {
            memset(w->block,0,4096);p16(sb+90,(uint16_t)g);p32(sb+1020,crc(UINT32_MAX,sb,1020));memcpy(w->block,sb,1024);
            r=write_block(t,g*32768,w->block,w);if (r) return r;
        }
    }
    r=t->flush(t->context);if (r) return r<0 ? r : -5;
    r=read_block(t,0,w->block);if (r) return r;
    p16(sb+90,0);p32(sb+1020,crc(UINT32_MAX,sb,1020));memcpy(w->block+1024,sb,1024);
    r=write_block(t,0,w->block,w);if (r) return r;
    r=t->flush(t->context);return r>0 ? -5 : r;
}
