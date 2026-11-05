#ifndef DOOM_LIBC_ERRNO_H
#define DOOM_LIBC_ERRNO_H

extern int errno;
int *__errno_location(void);

#define ENOENT  2
#define EIO     5
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EISDIR 21
#define EINVAL 22

#endif
