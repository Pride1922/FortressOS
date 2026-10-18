#include "common.h"
#include "gzip.h"

#ifdef GUNZIP_STANDALONE
#include "gzip.c"
#endif

int gunzip_main(int argc, char **argv) {
    return gzip_main_internal(argc, argv, true);
}
