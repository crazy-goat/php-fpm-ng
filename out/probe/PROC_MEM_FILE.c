#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
int main(void) { long v1 = (unsigned int) -1, v2 = 0; char buf[128]; int fd;
  snprintf(buf, sizeof(buf), "/proc/%d/mem", (int) getpid()); fd = open(buf, O_RDONLY);
  if (fd < 0) return 1; if (pread(fd, &v2, sizeof(long), (uintptr_t) &v1) != sizeof(long)) return 1;
  close(fd); return v1 != v2; }
