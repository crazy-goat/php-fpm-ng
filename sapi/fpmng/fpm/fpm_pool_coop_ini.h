/* fpm-ng: izolacja WARTOSCI wpisow ini (ini_set/ini_get) per request na
 * executorze coop (fiber). Patrz fpm_pool_coop_ini.c po uzasadnienie,
 * zakres i ograniczenia.
 */

#ifndef FPM_POOL_COOP_INI_H
#define FPM_POOL_COOP_INI_H 1

struct fpm_coop_req_s;

/* Wolac w fpm_coop_req_enter(), w bloku "if (ctx->live)": nakada z powrotem
 * WLASNA wartosc tego requestu na wpisy ini, ktore ten request zmienil przed
 * ostatnim zejsciem z procesora. Wolac PRZED wznowieniem/uruchomieniem
 * requestu. Tania sciezka: gdy ctx->ini_mods == NULL (request nigdy nie
 * ruszyl ini), nie robi NIC — ani jednej alokacji, ani przejscia po
 * jakiejkolwiek tablicy. */
void fpm_coop_ini_req_enter(struct fpm_coop_req_s *ctx);

/* Zwalnia to, co zostalo po requescie zniszczonym, gdy byl zdjety z procesora
 * (wpisy ini sa juz wtedy bazowe — przywrocil je fpm_coop_ini_req_leave). */
void fpm_coop_ini_req_free(struct fpm_coop_req_s *ctx);

/* Wolac w fpm_coop_req_leave(), w bloku "if (ctx->live)": zdejmuje z
 * EG(modified_ini_directives) wszystko, co TEN request zmienil od ostatniego
 * wejscia, chowa WLASNA wartosc requestu do ctx i przywraca kazdemu wpisowi
 * wartosc bazowa (ta, ktora widzialby kontener/inny request). Wolac PO
 * zejsciu requestu z procesora (zawieszenie — NIE koniec requestu, patrz
 * fpm_pool_coop.c: na koncu requestu ctx->live jest juz false, wiec ten
 * hook sie nie wola, a ostateczne odwikianie robi zend_ini_deactivate()).
 * Tania sciezka: gdy EG(modified_ini_directives) == NULL (request niczego
 * nie zmienil od ostatniego wejscia), nie robi NIC. */
void fpm_coop_ini_req_leave(struct fpm_coop_req_s *ctx);

#endif
