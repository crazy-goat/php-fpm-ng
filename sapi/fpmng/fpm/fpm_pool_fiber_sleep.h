/* fpm-ng: pool.executor = fiber — sleep()/usleep()/time_nanosleep() bez
 * blokowania calego procesu. SPIKE — patrz docs/spike-sleep-yield-report.md.
 *
 * Podmienia zif_handler trzech funkcji wewnetrznych w CG(function_table) na
 * wariant, ktory (gdy fpm_pool_fiber_can_wait() pozwala) zawiesza fiber
 * requestu przez fpm_pool_fiber_wait_wake() zamiast wolac prawdziwe
 * sleep()/usleep()/nanosleep() i blokowac cala petle zdarzen. Poza fiberem
 * requestu (albo gdy przelaczanie jest zablokowane) wola oryginalny handler
 * bez zadnej zmiany — prawdziwe blokujace zachowanie.
 *
 * CZEGO TO NIE OBEJMUJE (swiadomie, patrz raport):
 *  - pcntl_sleep() — dziala na SIGALRM/pauzie procesu, nie ma odpowiednika
 *    per-request w tym modelu (patrz fpm_pool_coop.c: funkcje pcntl.*
 *    procesowe i tak sa disable_functions w kontenerze fiber);
 *  - stream_select()/stream_socket_* z timeoutem na gniazdach spoza
 *    transportow tcp/unix — to juz teren fpm_pool_fiber_xport.c;
 *  - time_sleep_until() — nie ma jej w tablicy funkcji, gdy HAVE_NANOSLEEP
 *    jest wylaczone razem z time_nanosleep(); gdy jest, i tak pod spodem
 *    zapetla nanosleep() na EINTR — z braku realnego sygnalu w tym modelu
 *    nigdy by sie nie zapetlala, ale nie zostala pokryta w tym spike'u
 *    (nie ma jej w wymaganiach zadania — dopisac gdy zajdzie potrzeba,
 *    tym samym wzorcem co ponizej).
 */

#ifndef FPM_POOL_FIBER_SLEEP_H
#define FPM_POOL_FIBER_SLEEP_H 1

/* Wolac RAZ na dziecko poola fiber, po fpm_pool_fiber_xport_install().
 * Brakujace funkcje (np. nanosleep niedostepny na tej platformie) sa
 * pomijane z ostrzezeniem w logu — nigdy nie fatalnym bledem. */
void fpm_pool_fiber_sleep_install(void);

#endif
