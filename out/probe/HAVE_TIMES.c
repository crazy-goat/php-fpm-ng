#include <sys/times.h>
int main(void) { struct tms t; return times(&t) == (clock_t) -1; }
