/* fpm-ng: executor wielorequestowy (fiber) — wykrywanie zmian WCZYTANYCH
 * plikow na dysku, dyrektywa fiber.revalidate_freq.
 *
 * Problem: proces laduje aplikacje raz (tablice funkcji i klas sa procesowe,
 * z FPMNG_SHARED_INCLUDES=1 takze lista wczytanych plikow), a skrypt wejsciowy
 * czyta z dysku przy kazdym requescie. Po podmianie plikow bez reloadu dziala
 * nowy index.php na starym bootstrapie — cicho, bez bledu (docs/fiber_errors.md).
 *
 * Rozwiazanie: zapamietujemy mtime/rozmiar/inode kazdego pliku w chwili, gdy
 * silnik go kompiluje (hook zend_compile_file — to dokladnie ten moment,
 * w ktorym opened_path trafia do EG(included_files): compile_filename w
 * zend_language_scanner.l i zend_include_or_eval w zend_execute.c). Typ poola
 * cyklicznie (co fiber.revalidate_freq sekund, NIE per request) wola sweep(),
 * a gdy ktorys plik sie zmienil — grzecznie konczy workera; master go wymienia.
 *
 * Skrypt wejsciowy (file_handle->primary_script) jest pomijany: wykonujemy go
 * bezposrednio i czytamy z dysku przy kazdym requescie, jego zmiana nie wymaga
 * wymiany procesu. Koszt w sciezce requestu: jedno wyszukanie w tablicy per
 * kompilacja i jeden stat() per plik widziany PO RAZ PIERWSZY w procesie.
 */

#ifndef FPM_POOL_COOP_REVAL_H
#define FPM_POOL_COOP_REVAL_H 1

#include <stdbool.h>
#include <stddef.h>

/* Wlacza sledzenie i instaluje hook kompilacji. freq <= 0 = nic nie robi.
 * Wolac w dziecku, po fpm_coop_container_start(), przed pierwszym requestem. */
void fpm_coop_reval_start(int freq_seconds);

bool fpm_coop_reval_enabled(void);

/* Jeden przebieg: stat() kazdego zapamietanego pliku. 1 = ktorys sie zmienil
 * (path wskazuje na sciezke w naszej tablicy, why opisuje co sie zmienilo),
 * 0 = wszystko bez zmian. Plik, ktorego nie da sie zbadac (stat() bledny,
 * takze ENOENT), liczy sie jako zmieniony — deploy przez rename/rsync
 * przechodzi przez taki stan, a wynik (swiezy worker) jest wlasnie ten
 * pozadany. */
int fpm_coop_reval_sweep(const char **path, char *why, size_t why_len);

/* Statystyki do logow: ile plikow sledzimy, ile bylo przebiegow, ile
 * wywolan stat() od startu procesu (lacznie z rejestracja). */
void fpm_coop_reval_stats(unsigned *files, unsigned *sweeps, unsigned *stat_calls);

#endif
