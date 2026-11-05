/* FortressOS freestanding runtime adapter for Doom / doomgeneric */

#include "doom_runtime.h"

#ifdef DOOM_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* FortressOS Syscall constants */
#define SYS_EXIT           0
#define SYS_WRITE          1
#define SYS_OPEN           2
#define SYS_CLOSE          3
#define SYS_READ           4
#define SYS_MKDIR          11
#define SYS_UNLINK         12
#define SYS_RENAME         13
#define SYS_CLOCK_GETTIME  74
#define SYS_NANOSLEEP      75
#define SYS_FSTAT          78
#define SYS_MMAP           99

#define CLOCK_MONOTONIC    1

#define HEAP_ARENA_BASE    0x4000000000ULL
#define HEAP_ARENA_SIZE    (24ULL * 1024ULL * 1024ULL) /* 24 MiB */

#ifndef DOOM_HOST_TEST
int errno = 0;
int *__errno_location(void) { return &errno; }
#endif

static doom_runtime_stats_t g_stats;

void doom_runtime_get_stats(doom_runtime_stats_t *out) {
    if (out) {
        *out = g_stats;
    }
}

#ifndef DOOM_HOST_TEST

long doom_syscall(long num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    long ret;
    register uintptr_t r10 __asm__("r10") = a4;
    register uintptr_t r8  __asm__("r8")  = a5;
    register uintptr_t r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long sys3(long num, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return doom_syscall(num, a1, a2, a3, 0, 0, 0);
}

static inline long sys1(long num, uintptr_t a1) {
    return doom_syscall(num, a1, 0, 0, 0, 0, 0);
}

/* ========================================================================= */
/* Memory Allocator (Freelist arena over SYS_MMAP)                           */
/* ========================================================================= */

typedef struct block_hdr {
    size_t size;            /* Size of usable space following header */
    uint32_t is_free;       /* 1 if free, 0 if allocated */
    uint32_t magic;         /* 0xDO0M1993 */
    struct block_hdr *next;
    struct block_hdr *prev;
} block_hdr_t;

#define BLOCK_MAGIC 0xD0031993U
#define ALIGN16(x) (((x) + 15ULL) & ~15ULL)

static uint8_t *g_arena_start = NULL;
static size_t g_arena_total = 0;
static block_hdr_t *g_first_block = NULL;

static bool init_heap_arena(void) {
    if (g_arena_start) return true;

    long res = sys1(SYS_MMAP, HEAP_ARENA_SIZE);
    if (res < 0) {
        return false;
    }

    g_arena_start = (uint8_t *)(uintptr_t)res;
    g_arena_total = HEAP_ARENA_SIZE;

    g_first_block = (block_hdr_t *)g_arena_start;
    g_first_block->size = g_arena_total - sizeof(block_hdr_t);
    g_first_block->is_free = 1;
    g_first_block->magic = BLOCK_MAGIC;
    g_first_block->next = NULL;
    g_first_block->prev = NULL;

    g_stats.heap_arena_base = (uint64_t)(uintptr_t)g_arena_start;
    g_stats.heap_arena_size = g_arena_total;
    return true;
}

void *malloc(size_t size) {
    if (!size) return NULL;
    if (size > HEAP_ARENA_SIZE) return NULL;

    if (!init_heap_arena()) return NULL;

    size_t aligned_size = ALIGN16(size);
    block_hdr_t *curr = g_first_block;

    while (curr) {
        if (curr->magic != BLOCK_MAGIC) {
            /* Heap corruption */
            return NULL;
        }

        if (curr->is_free && curr->size >= aligned_size) {
            /* Can we split this block? */
            if (curr->size >= aligned_size + sizeof(block_hdr_t) + 32) {
                block_hdr_t *next_block = (block_hdr_t *)((uint8_t *)(curr + 1) + aligned_size);
                next_block->size = curr->size - aligned_size - sizeof(block_hdr_t);
                next_block->is_free = 1;
                next_block->magic = BLOCK_MAGIC;
                next_block->next = curr->next;
                next_block->prev = curr;
                if (curr->next) {
                    curr->next->prev = next_block;
                }
                curr->next = next_block;
                curr->size = aligned_size;
            }

            curr->is_free = 0;
            g_stats.heap_alloc_count++;
            g_stats.heap_allocated_current += curr->size;
            if (g_stats.heap_allocated_current > g_stats.heap_allocated_peak) {
                g_stats.heap_allocated_peak = g_stats.heap_allocated_current;
            }
            return (void *)(curr + 1);
        }

        curr = curr->next;
    }

    /* Out of memory */
    return NULL;
}

void free(void *ptr) {
    if (!ptr) return;

    block_hdr_t *hdr = ((block_hdr_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC || hdr->is_free) {
        return;
    }

    hdr->is_free = 1;
    g_stats.heap_free_count++;
    if (g_stats.heap_allocated_current >= hdr->size) {
        g_stats.heap_allocated_current -= hdr->size;
    } else {
        g_stats.heap_allocated_current = 0;
    }

    /* Coalesce forward */
    if (hdr->next && hdr->next->is_free) {
        hdr->size += sizeof(block_hdr_t) + hdr->next->size;
        hdr->next = hdr->next->next;
        if (hdr->next) {
            hdr->next->prev = hdr;
        }
    }

    /* Coalesce backward */
    if (hdr->prev && hdr->prev->is_free) {
        block_hdr_t *prev = hdr->prev;
        prev->size += sizeof(block_hdr_t) + hdr->size;
        prev->next = hdr->next;
        if (hdr->next) {
            hdr->next->prev = prev;
        }
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (!nmemb || !size) return NULL;
    if (nmemb > SIZE_MAX / size) return NULL;

    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) {
        free(ptr);
        return NULL;
    }

    block_hdr_t *hdr = ((block_hdr_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC) return NULL;

    if (hdr->size >= size) {
        return ptr;
    }

    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, hdr->size);
    free(ptr);
    return new_ptr;
}

/* ========================================================================= */
/* File I/O (Stdio over FortressOS VFS syscalls)                             */
/* ========================================================================= */

#define MAX_OPEN_FILES 16
#define FILE_PATH_MAX  128

struct doom_file {
    int fd;
    long offset;
    long file_size;
    bool is_open;
    bool is_write;
    bool is_eof;
    char path[FILE_PATH_MAX];
};

static struct doom_file g_files[MAX_OPEN_FILES];
static struct doom_file g_stdio_in  = {.fd = 0, .is_open = true};
static struct doom_file g_stdio_out = {.fd = 1, .is_open = true, .is_write = true};
static struct doom_file g_stdio_err = {.fd = 2, .is_open = true, .is_write = true};

FILE *stdin  = &g_stdio_in;
FILE *stdout = &g_stdio_out;
extern FILE *stderr;
FILE *stderr = &g_stdio_err;

typedef struct {
    uint32_t size;        /* 40 */
    uint32_t version;     /* 1 */
    uint64_t file_size;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t mnt_flags;
    uint32_t reserved;
} stat_ext_v1_internal_t;

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;

    int slot = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!g_files[i].is_open) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        errno = ENOMEM;
        return NULL;
    }

    bool write_mode = false;
    uint64_t flags = O_RDONLY;
    if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT;
        write_mode = true;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT;
        write_mode = true;
    }

    long fd = sys3(SYS_OPEN, (uintptr_t)path, flags, 0644);
    if (fd < 0) {
        errno = ENOENT;
        return NULL;
    }

    stat_ext_v1_internal_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.version = 1;
    long fstat_res = sys3(SYS_FSTAT, (uintptr_t)fd, (uintptr_t)&st, 0);
    long fsize = 0;
    if (fstat_res >= 0) {
        fsize = (long)st.file_size;
    }

    struct doom_file *f = &g_files[slot];
    f->fd = (int)fd;
    f->offset = 0;
    f->file_size = fsize;
    f->is_open = true;
    f->is_write = write_mode;
    f->is_eof = false;

    size_t plen = strlen(path);
    if (plen >= FILE_PATH_MAX) plen = FILE_PATH_MAX - 1;
    memcpy(f->path, path, plen);
    f->path[plen] = '\0';

    g_stats.file_open_count++;
    return f;
}

int fclose(FILE *stream) {
    if (!stream || !stream->is_open) return EOF;
    if (stream == stdin || stream == stdout || stream == stderr) return 0;

    sys1(SYS_CLOSE, (uintptr_t)stream->fd);
    stream->is_open = false;
    stream->fd = -1;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !size || !nmemb || !stream || !stream->is_open) return 0;

    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    uint8_t *buf = (uint8_t *)ptr;

    while (total_read < bytes_to_read) {
        long n = sys3(SYS_READ, (uintptr_t)stream->fd, (uintptr_t)(buf + total_read),
                     bytes_to_read - total_read);
        if (n <= 0) {
            stream->is_eof = true;
            break;
        }
        total_read += (size_t)n;
        stream->offset += (long)n;
    }

    g_stats.file_read_bytes += total_read;
    return total_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !size || !nmemb || !stream || !stream->is_open) return 0;

    size_t bytes_to_write = size * nmemb;
    size_t total_written = 0;
    const uint8_t *buf = (const uint8_t *)ptr;

    while (total_written < bytes_to_write) {
        long n = sys3(SYS_WRITE, (uintptr_t)stream->fd, (uintptr_t)(buf + total_written),
                      bytes_to_write - total_written);
        if (n <= 0) break;
        total_written += (size_t)n;
        stream->offset += (long)n;
        if (stream->offset > stream->file_size) {
            stream->file_size = stream->offset;
        }
    }

    return total_written / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream || !stream->is_open) return -1;

    g_stats.file_seek_count++;

    long target = 0;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = stream->offset + offset;
    } else if (whence == SEEK_END) {
        target = stream->file_size + offset;
    } else {
        return -1;
    }

    if (target < 0) return -1;
    if (target == stream->offset) return 0;

    if (target > stream->offset) {
        /* Forward seek: read and discard */
        long to_skip = target - stream->offset;
        uint8_t discard[256];
        while (to_skip > 0) {
            size_t chunk = to_skip > (long)sizeof(discard) ? sizeof(discard) : (size_t)to_skip;
            size_t r = fread(discard, 1, chunk, stream);
            if (r == 0) return -1;
            to_skip -= (long)r;
        }
        return 0;
    }

    /* Backward seek: rewind via reopen and advance */
    g_stats.file_backward_seek_count++;
    sys1(SYS_CLOSE, (uintptr_t)stream->fd);

    long new_fd = sys3(SYS_OPEN, (uintptr_t)stream->path, O_RDONLY, 0);
    if (new_fd < 0) {
        stream->is_open = false;
        return -1;
    }

    stream->fd = (int)new_fd;
    stream->offset = 0;
    stream->is_eof = false;

    if (target > 0) {
        long to_skip = target;
        uint8_t discard[256];
        while (to_skip > 0) {
            size_t chunk = to_skip > (long)sizeof(discard) ? sizeof(discard) : (size_t)to_skip;
            size_t r = fread(discard, 1, chunk, stream);
            if (r == 0) return -1;
            to_skip -= (long)r;
        }
    }

    return 0;
}

long ftell(FILE *stream) {
    if (!stream || !stream->is_open) return -1;
    return stream->offset;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int remove(const char *pathname) {
    if (!pathname) return -1;
    long r = sys1(SYS_UNLINK, (uintptr_t)pathname);
    return r >= 0 ? 0 : -1;
}

int rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;
    long r = sys3(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath, 0);
    return r >= 0 ? 0 : -1;
}

int mkdir(const char *pathname, mode_t mode) {
    if (!pathname) return -1;
    long r = sys3(SYS_MKDIR, (uintptr_t)pathname, (uintptr_t)mode, 0);
    return r >= 0 ? 0 : -1;
}

/* ========================================================================= */
/* Process, Time & Exit                                                      */
/* ========================================================================= */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_internal_t;

int usleep(unsigned int usec) {
    timespec_internal_t req;
    req.tv_sec = usec / 1000000;
    req.tv_nsec = (usec % 1000000) * 1000;
    sys3(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0);
    return 0;
}

int close(int fd) {
    return (int)sys1(SYS_CLOSE, (uintptr_t)fd);
}

void exit(int status) {
    sys1(SYS_EXIT, (uintptr_t)status);
    while (1) {
        __asm__ volatile("hlt");
    }
}

int system(const char *command) {
    (void)command;
    /* Subprocess execution through shell is disabled */
    return -1;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

void __doom_assert_fail(const char *expr, const char *file, int line) {
    printf("[DOOM ASSERT FAIL] %s:%d: %s\n", file, line, expr);
    exit(134);
}

/* ========================================================================= */
/* Formatting & Output (printf / snprintf)                                   */
/* ========================================================================= */

static size_t format_u64(char *buf, uint64_t val, int base, bool uppercase) {
    char tmp[32];
    size_t tpos = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!base) base = 10;
    do {
        tmp[tpos++] = digits[val % (unsigned)base];
        val /= (unsigned)base;
    } while (val > 0);

    for (size_t i = 0; i < tpos; i++) {
        buf[i] = tmp[tpos - 1 - i];
    }
    buf[tpos] = '\0';
    return tpos;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    if (!format) return 0;

    size_t written = 0;
    const char *p = format;

    while (*p) {
        if (*p != '%') {
            if (str && written + 1 < size) {
                str[written] = *p;
            }
            written++;
            p++;
            continue;
        }

        p++; /* Skip '%' */
        if (*p == '%') {
            if (str && written + 1 < size) {
                str[written] = '%';
            }
            written++;
            p++;
            continue;
        }

        /* Check width/flags */
        int width = 0;
        bool zero_pad = false;
        if (*p == '0') {
            zero_pad = true;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        if (*p == 'l') {
            p++;
            if (*p == 'l') p++;
        }

        char num_buf[32];
        size_t num_len = 0;

        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t slen = strlen(s);
            for (size_t i = 0; i < slen; i++) {
                if (str && written + 1 < size) {
                    str[written] = s[i];
                }
                written++;
            }
            break;
        }
        case 'd':
        case 'i': {
            int64_t val = (int64_t)va_arg(ap, int);
            if (val < 0) {
                if (str && written + 1 < size) {
                    str[written] = '-';
                }
                written++;
                val = -val;
            }
            num_len = format_u64(num_buf, (uint64_t)val, 10, false);
            while (width > (int)num_len) {
                if (str && written + 1 < size) {
                    str[written] = zero_pad ? '0' : ' ';
                }
                written++;
                width--;
            }
            for (size_t i = 0; i < num_len; i++) {
                if (str && written + 1 < size) {
                    str[written] = num_buf[i];
                }
                written++;
            }
            break;
        }
        case 'u': {
            uint64_t val = (uint64_t)va_arg(ap, unsigned int);
            num_len = format_u64(num_buf, val, 10, false);
            while (width > (int)num_len) {
                if (str && written + 1 < size) {
                    str[written] = zero_pad ? '0' : ' ';
                }
                written++;
                width--;
            }
            for (size_t i = 0; i < num_len; i++) {
                if (str && written + 1 < size) {
                    str[written] = num_buf[i];
                }
                written++;
            }
            break;
        }
        case 'x':
        case 'X': {
            uint64_t val = (uint64_t)va_arg(ap, unsigned int);
            num_len = format_u64(num_buf, val, 16, *p == 'X');
            while (width > (int)num_len) {
                if (str && written + 1 < size) {
                    str[written] = zero_pad ? '0' : ' ';
                }
                written++;
                width--;
            }
            for (size_t i = 0; i < num_len; i++) {
                if (str && written + 1 < size) {
                    str[written] = num_buf[i];
                }
                written++;
            }
            break;
        }
        case 'p': {
            uintptr_t val = (uintptr_t)va_arg(ap, void *);
            if (str && written + 1 < size) str[written] = '0';
            written++;
            if (str && written + 1 < size) str[written] = 'x';
            written++;
            num_len = format_u64(num_buf, (uint64_t)val, 16, false);
            for (size_t i = 0; i < num_len; i++) {
                if (str && written + 1 < size) str[written] = num_buf[i];
                written++;
            }
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (str && written + 1 < size) {
                str[written] = c;
            }
            written++;
            break;
        }
        default:
            if (*p) {
                if (str && written + 1 < size) str[written] = *p;
                written++;
            }
            break;
        }
        if (*p) p++;
    }

    if (str && size > 0) {
        if (written < size) {
            str[written] = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }

    return (int)written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(str, size, format, ap);
    va_end(ap);
    return r;
}

int sscanf(const char *str, const char *format, ...) {
    if (!str || !format) return 0;
    va_list ap;
    va_start(ap, format);
    int matched = 0;
    const char *s = str;
    const char *f = format;

    while (*f && *s) {
        if (*f == ' ') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            f++;
            continue;
        }
        if (*f != '%') {
            if (*f == *s) { f++; s++; }
            else break;
            continue;
        }

        f++; /* skip '%' */
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

        if (*f == 'd' || *f == 'i') {
            int sign = 1;
            if (*s == '-') { sign = -1; s++; }
            else if (*s == '+') { s++; }
            int base = 10;
            if (*f == 'i' && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                base = 16;
                s += 2;
            } else if (*f == 'i' && s[0] == '0') {
                base = 8;
                s++;
            }
            int val = 0;
            bool read_any = false;
            while (*s) {
                int d = -1;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                if (d < 0 || d >= base) break;
                val = val * base + d;
                read_any = true;
                s++;
            }
            if (!read_any) break;
            int *out = va_arg(ap, int *);
            if (out) *out = sign * val;
            matched++;
            f++;
        } else if (*f == 'u') {
            unsigned int val = 0;
            bool read_any = false;
            while (*s >= '0' && *s <= '9') {
                val = val * 10 + (unsigned int)(*s - '0');
                read_any = true;
                s++;
            }
            if (!read_any) break;
            unsigned int *out = va_arg(ap, unsigned int *);
            if (out) *out = val;
            matched++;
            f++;
        } else if (*f == 'x' || *f == 'X') {
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            unsigned int val = 0;
            bool read_any = false;
            while (*s) {
                int d = -1;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                if (d < 0) break;
                val = val * 16 + (unsigned int)d;
                read_any = true;
                s++;
            }
            if (!read_any) break;
            unsigned int *out = va_arg(ap, unsigned int *);
            if (out) *out = val;
            matched++;
            f++;
        } else if (*f == 'o') {
            if (*s == '0') s++;
            unsigned int val = 0;
            bool read_any = false;
            while (*s >= '0' && *s <= '7') {
                val = val * 8 + (unsigned int)(*s - '0');
                read_any = true;
                s++;
            }
            if (!read_any) break;
            unsigned int *out = va_arg(ap, unsigned int *);
            if (out) *out = val;
            matched++;
            f++;
        } else {
            break;
        }
    }

    va_end(ap);
    return matched;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(str, 4096, format, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    if (n > 0) {
        int fd = stream ? stream->fd : 1;
        sys3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    }
    return n;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vfprintf(stdout, format, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    if (!s) return EOF;
    size_t len = strlen(s);
    sys3(SYS_WRITE, (uintptr_t)1, (uintptr_t)s, len);
    sys3(SYS_WRITE, (uintptr_t)1, (uintptr_t)"\n", 1);
    return (int)len + 1;
}

int putchar(int c) {
    char ch = (char)c;
    sys3(SYS_WRITE, (uintptr_t)1, (uintptr_t)&ch, 1);
    return c;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    sys3(SYS_WRITE, (uintptr_t)stream->fd, (uintptr_t)s, len);
    return (int)len;
}

/* ========================================================================= */
/* String & Memory Primitives                                                */
/* ========================================================================= */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || !s1[i]) {
            return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        }
    }
    return 0;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = *s1 >= 'A' && *s1 <= 'Z' ? *s1 + 32 : *s1;
        int c2 = *s2 >= 'A' && *s2 <= 'Z' ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int c1 = s1[i] >= 'A' && s1[i] <= 'Z' ? s1[i] + 32 : s1[i];
        int c2 = s2[i] >= 'A' && s2[i] <= 'Z' ? s2[i] + 32 : s2[i];
        if (c1 != c2 || !s1[i]) return c1 - c2;
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    }
    return NULL;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == 0) ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack, *n = needle;
            while (*h && *n && (*h == *n)) { h++; n++; }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

int atoi(const char *nptr) {
    if (!nptr) return 0;
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    int sign = 1;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    int val = 0;
    while (*nptr >= '0' && *nptr <= '9') {
        val = val * 10 + (*nptr - '0');
        nptr++;
    }
    return sign * val;
}

double atof(const char *nptr) {
    if (!nptr) return 0.0;
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    double sign = 1.0;
    if (*nptr == '-') { sign = -1.0; nptr++; }
    else if (*nptr == '+') { nptr++; }
    double val = 0.0;
    while (*nptr >= '0' && *nptr <= '9') {
        val = val * 10.0 + (double)(*nptr - '0');
        nptr++;
    }
    if (*nptr == '.') {
        nptr++;
        double frac = 0.1;
        while (*nptr >= '0' && *nptr <= '9') {
            val += (double)(*nptr - '0') * frac;
            frac *= 0.1;
            nptr++;
        }
    }
    return sign * val;
}

int abs(int j) {
    return j < 0 ? -j : j;
}

#endif /* !DOOM_HOST_TEST */

void doom_runtime_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}
