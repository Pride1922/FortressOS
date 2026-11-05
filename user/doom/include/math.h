#ifndef DOOM_LIBC_MATH_H
#define DOOM_LIBC_MATH_H

static inline double fabs(double x) {
    return x < 0.0 ? -x : x;
}

#endif
