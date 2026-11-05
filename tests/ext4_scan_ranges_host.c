#define FORTRESS_THREAD_H
#define main original_mount_main
#include "ext4_mount_host.c"
#undef main
static void check_group_reservations(void) {
    e4_range_t ranges[256];
    ext4_mount_t fs = {.first = 1, .blocks = 1025, .reserved = ranges};
    for (unsigned group = 0; group < 32; ++group) {
        unsigned first = fs.ranges;
        uint32_t start = 1 + group * 32;
        assert(e4_reserve(&fs, start + 20, 4, first));
        assert(e4_reserve(&fs, start + 2, 1, first));
        assert(e4_reserve(&fs, start + 10, 1, first));
        for (uint32_t lo = start; lo < start + 32; ++lo) {
            for (uint32_t len = 1; len <= 5 && len <= start + 32 - lo; ++len) {
                bool expected = true;
                for (unsigned i = 0; i < fs.ranges; ++i)
                    if (lo < ranges[i].lo + ranges[i].len && ranges[i].lo < lo + len)
                        expected = false;
                unsigned before = fs.ranges;
                assert(e4_reserve(&fs, lo, len, first) == expected);
                assert(fs.ranges == before + expected);
                fs.ranges = before;
            }
        }
    }
    puts("PASS group reservations: unsorted metadata, overlap and boundary rejection against full-range oracle");
}
int main(void) {
 check_group_reservations();
 ext4_mount_t fs={0};e4_range_t ranges[256];fs.reserved=ranges;
 for(unsigned count=0;count<=256;count++){
  fs.ranges=count;
  for(unsigned i=0;i<count;i++)ranges[i]=(e4_range_t){i*13+1,3};
  for(unsigned start=0;start<3400;start++){
   unsigned expected=0;while(expected<count && ranges[expected].lo<start)expected++;
   assert(e4_reserved_first(&fs,start)==expected);
  }
  assert(e4_reserved_first(&fs,UINT32_MAX)==count);
 }
 puts("PASS reservation search: empty/full maps, every boundary and gap, UINT32_MAX, differential linear oracle");return 0;
}
