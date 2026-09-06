/* fpm-ng: glue metryk aplikacyjnych (NOTES 3k) po stronie SAPI.
 *
 * Master: po sparsowaniu konfiguracji (pm.max_children wszystkich poolow
 * znane) liczy calkowita liczbe slotow workera i alokuje JEDEN region w
 * pamieci dzielonej przez fpm_shm_alloc, ktory przekazuje do rozszerzenia
 * fpmng_metrics (fpmng_metrics_shm_init). Dzieci dziedzicza mapowanie po
 * forku.
 *
 * Dziecko: w run_child: (fpm.c) oblicza swój globalny indeks slotu i nazwe
 * poola i przekazuje je do fpmng_metrics_child_attach. Od tej chwili
 * funkcje PHP fpm_metric_* pisza do wlasnej tablicy serii.
 *
 * Slot workera = suma pm.max_children poolow wczesniejszych w kolejnosci
 * configu + indeks ze scoreboardu wlasnego poola. Indeks, nie pid —
 * recykling po pm.max_requests nie zeruje wtedy licznikow (NOTES 3k).
 */

#ifndef FPM_METRICS_H
#define FPM_METRICS_H 1

/* Master, po fpm_conf_init_main(), przed forkiem dzieci. Porazka NIE
 * zabija FPM (metryki aplikacyjne sa dodatkiem, nie fundamentem) —
 * zwraca -1 i loguje; wtedy wszystkie fpm_metric_* zwracaja false. */
int fpm_metrics_init_main(void);

/* Dziecko workera, w run_child: PRZED child_main i PRZED
 * fpm_cleanups_run(CHILD) — potem lista poolow i config znikaja, a my
 * potrzebujemy pm.max_children poolow wczesniejszych i nazwy wlasnego. */
void fpm_metrics_child_init(void);

#endif
