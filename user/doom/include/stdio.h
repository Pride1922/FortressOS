#ifndef DOOM_LIBC_STDIO_H
#define DOOM_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct doom_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vfprintf(FILE *stream, const char *format, va_list ap);

int puts(const char *s);
int putchar(int c);
int fputs(const char *s, FILE *stream);
int fgetc(FILE *stream);
int ungetc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int feof(FILE *stream);
int fscanf(FILE *stream, const char *format, ...);

int sscanf(const char *str, const char *format, ...);

#endif
