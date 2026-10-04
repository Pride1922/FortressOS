#ifndef SHELL_HISTORY_PERSIST_H
#define SHELL_HISTORY_PERSIST_H

#include "types.h"

#define HISTORY_AUTOFLUSH_INTERVAL 5

bool history_save(void);
bool history_load(void);

void history_mark_dirty(void);
void history_autoflush_maybe(void);

bool history_is_dirty(void);
uint32_t history_get_counter(void);
bool history_is_warned(void);

#endif /* SHELL_HISTORY_PERSIST_H */
