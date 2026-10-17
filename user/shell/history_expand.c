#include "history_expand.h"

static bool space(char c) { return c==' ' || c=='\t'; }

int history_expand_sudo(const char *line, const char *previous, char *out, size_t cap) {
    while (space(*line)) line++;
    const char *word="sudo";
    while (*word) { if (*line++!=*word++) return 0; }
    if (!space(*line)) return 0;
    while (space(*line)) line++;
    if (*line++!='!') return 0;
    if (*line++!='!') return 0;
    while (space(*line)) line++;
    if (*line) return 0;
    if (!previous || !*previous) return -1;
    size_t n=0;
    while (previous[n]) n++;
    if (cap<6 || n>cap-6) return -2;
    const char prefix[]="sudo ";
    for (size_t i=0;i<5;i++) out[i]=prefix[i];
    for (size_t i=0;i<=n;i++) out[5+i]=previous[i];
    return 1;
}
