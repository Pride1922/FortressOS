#include "types.h"
uint64_t ext4_time_test_seconds;
/* Strong provider overrides the filesystem's weak host-only clock stub. */
uint64_t time_get_epoch_seconds(void) { return ext4_time_test_seconds; }
