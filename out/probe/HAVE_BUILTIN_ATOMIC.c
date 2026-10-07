int main(void) { int v = 1; return (__sync_bool_compare_and_swap(&v, 1, 2) && __sync_add_and_fetch(&v, 1)) ? 1 : 0; }
