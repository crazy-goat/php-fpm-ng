#include <sys/select.h>
int main(void) { fd_set s; FD_ZERO(&s); return select(0, &s, 0, 0, 0); }
