#include <sys/epoll.h>
int main(void) { struct epoll_event e; (void) e; return epoll_create(1) < 0; }
