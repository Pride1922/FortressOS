#define FORTRESS_THREAD_H
#define main original_mount_main
#include "ext4_mount_host.c"
#undef main
int main(void) {
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
