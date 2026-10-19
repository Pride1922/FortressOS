#ifndef FORTRESS_EXT4_TIME_H
#define FORTRESS_EXT4_TIME_H
#include "types.h"

enum ext4_time_field { EXT4_ATIME, EXT4_CTIME, EXT4_MTIME, EXT4_CRTIME };
enum {
    EXT4_TIME_ATIME = 8, EXT4_TIME_CTIME = 12, EXT4_TIME_MTIME = 16,
    EXT4_TIME_EXTRA_ISIZE = 128, EXT4_TIME_CTIME_EXTRA = 132,
    EXT4_TIME_MTIME_EXTRA = 136, EXT4_TIME_ATIME_EXTRA = 140,
    EXT4_TIME_CRTIME = 144, EXT4_TIME_CRTIME_EXTRA = 148
};

/* ext4: signed low 32-bit seconds plus two unsigned epoch bits. */
static inline bool ext4_time_encode(int64_t seconds, uint32_t nanos,
                                    uint32_t *low, uint32_t *extra) {
    if (!low || !extra || seconds < -2147483648LL || seconds > 15032385535LL ||
        nanos >= 1000000000u) return false;
    uint32_t lo = (uint32_t)seconds;
    int64_t signed_low = (lo & 0x80000000u) ? (int64_t)lo - 4294967296LL : lo;
    uint32_t epoch = (uint32_t)((seconds - signed_low) / 4294967296LL);
    *low = lo;
    *extra = (nanos << 2) | epoch;
    return true;
}
static inline void ext4_time_put32(uint8_t *p, uint32_t value) {
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
    p[2]=(uint8_t)(value>>16);p[3]=(uint8_t)(value>>24);
}
/* No writes on failure. Never use bytes beyond the inode's declared extra area.
 * Missing extension permits signed-32 seconds only, without fractional time. */
static inline bool ext4_inode_time_set(uint8_t raw[256], enum ext4_time_field field,
                                       int64_t seconds, uint32_t nanos) {
    static const unsigned base[]={EXT4_TIME_ATIME,EXT4_TIME_CTIME,EXT4_TIME_MTIME,EXT4_TIME_CRTIME};
    static const unsigned extension[]={EXT4_TIME_ATIME_EXTRA,EXT4_TIME_CTIME_EXTRA,
        EXT4_TIME_MTIME_EXTRA,EXT4_TIME_CRTIME_EXTRA};
    if (!raw || (unsigned)field>EXT4_CRTIME) return false;
    unsigned size=(unsigned)raw[EXT4_TIME_EXTRA_ISIZE]|
                  (unsigned)raw[EXT4_TIME_EXTRA_ISIZE+1]<<8;
    if (size<4 || size>128 || size%4 || base[field]+4>128+size) return false;
    bool extended=extension[field]+4<=128+size;
    if (!extended && (seconds< -2147483648LL || seconds>2147483647LL || nanos))return false;
    uint32_t low,extra;
    if (!ext4_time_encode(seconds,nanos,&low,&extra)) return false;
    ext4_time_put32(raw+base[field],low);
    if(extended)ext4_time_put32(raw+extension[field],extra);
    return true;
}
#endif
