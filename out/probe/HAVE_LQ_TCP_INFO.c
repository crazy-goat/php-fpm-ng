#include <netinet/tcp.h>
int main(void) { struct tcp_info ti; int x = TCP_INFO; (void) ti; return x < 0; }
