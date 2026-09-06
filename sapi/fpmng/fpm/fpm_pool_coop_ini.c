/* fpm-ng: izolacja WARTOSCI wpisow ini (ini_set/ini_get) per request na
 * executorze coop (fiber).
 *
 * Problem (zmierzony na Symfony, endpoint zwracajacy ini_get('session.save_handler')
 * pod 8 rownoleglymi requestami): fpm_pool_coop.c robi JEDEN php_request_startup()
 * na proces kontenera, wiec EG(ini_directives) — tablica zend_ini_entry —
 * jest DZIELONA przez wszystkie requesty w locie. ini_set() jednego requestu
 * pisze wprost do ini_entry->value, ktory jest ten sam dla kazdego innego
 * requestu, dopoki wszystkie sa zawieszone na tym samym procesie. Dotad
 * przywracalismy zmienione wpisy do wartosci bazowej dopiero na KONCU
 * requestu (zend_ini_deactivate() w fpm_pool_coop.c) — za pozno: requesty
 * WSPOLBIEZNE (inny fiber w locie na tym samym procesie) widza ini_set()
 * requestu, ktory jeszcze trwa.
 *
 * Rozwiazanie: dokladnie ten sam wzorzec co SG/OG/symbol_table w
 * fpm_pool_coop.c i ps_globals w fpm_pool_coop_session.c — swap stanu przy
 * kazdym wejsciu/zejsciu z procesora. Tutaj "stanem" jest WYLACZNIE
 * ini_entry->value (i towarzyszace mu pola modified/orig_value/modifiable)
 * dla wpisow, ktore TEN request zmienil — NIE caly EG(ini_directives)
 * (dzielona ze wszystkimi, kopiowanie calej tablicy przy kazdym przelaczeniu
 * fibera byloby bez sensu drogie).
 *
 * --- Dlaczego EG(modified_ini_directives) wystarcza za liste do przejscia -
 *
 * zend_alter_ini_entry_ex (Zend/zend_ini.c) dopisuje kazdy zmieniony wpis do
 * EG(modified_ini_directives) (nazwa -> zend_ini_entry*) i ustawia na wpisie
 * modified=true, orig_value=wartosc SPRZED tej zmiany — ale TYLKO przy
 * PIERWSZEJ zmianie (if (!modified) {...}); kolejne ini_set() na tym samym
 * kluczu, w tej samej "rundzie modyfikacji", nie ruszaja orig_value. Skoro w
 * modelu coop w danej chwili wykonuje sie NAJWYZEJ JEDEN fiber (kooperacyjne
 * przelaczanie, nie watki), a MY dbamy o to, zeby KAZDE zejscie requestu z
 * procesora w pelni przywracalo zmienione wpisy do stanu bazowego i zerowalo
 * modified — to orig_value, ktory zend_alter_ini_entry_ex zobaczy przy
 * NASTEPNEJ modyfikacji (czy to tego samego requestu po wznowieniu, czy
 * innego requestu, ktory akurat dostanie procesor), zawsze jest prawdziwa
 * wartosc bazowa (config poola/php_admin_value), nigdy cudza wartosc w
 * locie. Innymi slowy: EG(modified_ini_directives) w tym modelu jest
 * DOKLADNIE lista "co TEN wlasnie dzialajacy request zmienil od swojego
 * ostatniego wejscia" — pod warunkiem, ze nikt inny nie zostawia
 * modified=true na wyjsciu. Ten plik jest tym warunkiem.
 *
 * --- Co przenosimy i jak (transfer wlasnosci, bez refcountow) --------------
 *
 * Przy zejsciu (fpm_coop_ini_req_leave): dla kazdego wpisu w
 * EG(modified_ini_directives) chowamy JEGO BIEZACA wartosc (wlasna wartosc
 * TEGO requestu, np. "5" po ini_set('precision', '5')) do ctx->ini_values
 * (nazwa -> zend_string*, PRZENIESIENIE wskaznika, bez zend_string_copy/
 * release — jak memcpy SG/OG w fpm_pool_coop.c), przywracamy
 * ini_entry->value = ini_entry->orig_value (baza), modified = false,
 * orig_value = NULL, modifiable = orig_modifiable. Cala tablice
 * EG(modified_ini_directives) przenosimy en bloc do ctx->ini_mods (to zwykla
 * podmiana wskaznika — HashTable* i tak trzyma tylko zend_ini_entry*, ktore
 * nie naleza do nas) i zerujemy EG(modified_ini_directives), zeby nastepny
 * request na tym procesie zaczynal od czystego "nic nie zmienione".
 *
 * Przy wejsciu (fpm_coop_ini_req_enter): dla kazdego wpisu w ctx->ini_mods
 * odtwarzamy dokladnie to, co zend_alter_ini_entry_ex zrobilby SAM: biezaca
 * (bazowa) wartosc wpisu staje sie orig_value, wlasna wartosc requestu z
 * ctx->ini_values wraca do ini_entry->value, modified = true. Tablice
 * ctx->ini_mods oddajemy z powrotem jako EG(modified_ini_directives)
 * (znowu podmiana wskaznika) — dzieki temu zarowno kolejne ini_set() W TYM
 * requescie, jak i finalny zend_ini_deactivate() na koncu requestu
 * (fpm_pool_coop.c) dzialaja dokladnie tak, jakby nic sie nie stalo.
 *
 * CELOWO NIE wolamy tu ini_entry->on_modify() przy zadnym przelaczeniu —
 * patrz nizej, sekcja "Czego ta izolacja NIE naprawia".
 *
 * --- Tania sciezka -----------------------------------------------------------
 *
 * Request, ktory nie ruszal ini od ostatniego wejscia, ma
 * EG(modified_ini_directives) == NULL — dokladnie tak samo jak dzis w
 * zend_ini_deactivate(). fpm_coop_ini_req_leave() sprawdza to jako PIERWSZE
 * i wraca bez alokacji i bez przejscia po jakiejkolwiek tablicy.
 * fpm_coop_ini_req_enter() sprawdza ctx->ini_mods == NULL rownie tanio. To
 * jest sciezka KAZDEGO przelaczenia fibera, ktory nie uzywa ini_set/
 * set_time_limit/session_set_save_handler/... — czyli zdecydowanej
 * wiekszosci przelaczen.
 *
 * --- Czego ta izolacja NIE naprawia (powiedziane wprost) -------------------
 *
 * Wiele on_modify (Zend/zend_ini.c: OnUpdateLong/OnUpdateBool/...) NIE tylko
 * ustawia ini_entry->value — kopiuje tez sparsowana wartosc do PROCESOWEGO
 * pola przez ZEND_INI_GET_ADDR (mh_arg1 = offset, mh_arg2 = adres bazy
 * globali modulu; w buildzie NTS globale modulu sa JEDNĄ instancja na
 * caly proces, tak jak core_globals). Ten plik swapuje WYLACZNIE
 * ini_entry->value/orig_value/modified — NIE wola on_modify przy
 * przelaczeniu (patrz uzasadnienie wyzej: bezpieczenstwo i koszt), wiec
 * TO POLE PROCESOWE nie jest przelaczane. Przyklad: ini_set('precision', '5')
 * poprawi ini_get('precision') WYLACZNIE dla tego requestu (naprawione tu),
 * ale realna precyzja uzywana przez var_dump/serialize (core_globals.precision,
 * ustawiane przez OnUpdateLong) zostanie procesowa, dopoki jest live —
 * czyli moze przeciekac do innych requestow w locie mimo tej naprawy.
 * Jedyny wyjatek w tej bazie kodu to ext/session: PS(mod) (ustawiane przez
 * OnUpdateSaveHandler) jest bezpieczne, bo cale ps_globals (w tym PS(mod))
 * jest JUZ swapowane osobno, per request, przez fpm_pool_coop_session.c —
 * niezaleznie od tego pliku. Naprawienie ogolnego przypadku wymagaloby
 * swapowania globali KAZDEGO modulu z on_modify (jak ps_globals) — nie tego
 * dotyczy to zgloszenie (zmierzony objaw to konkretnie ini_get() widzi cudza
 * wartosc), wiec tu sie zatrzymujemy i mowimy to wprost zamiast udawac
 * pelna izolacje.
 */

#include "fpm_config.h"

#include "php.h"
#include "zend_ini.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_ini.h"

void fpm_coop_ini_req_leave(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_ini_entry *entry;
	zend_string *name;
	HashTable *values;

	if (!EG(modified_ini_directives)) {
		return;
	}

	ALLOC_HASHTABLE(values);
	zend_hash_init(values, zend_hash_num_elements(EG(modified_ini_directives)), NULL, NULL, false);

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(EG(modified_ini_directives), name, entry) {
		/* Wlasna wartosc TEGO requestu — chowamy ja, transfer wskaznika. */
		zend_hash_add_ptr(values, name, entry->value);

		/* Baza wraca na wpis, dokladnie jak zend_restore_ini_entry_cb —
		 * poza wolaniem on_modify, patrz uzasadnienie na gorze pliku. */
		entry->value = entry->orig_value;
		entry->modifiable = entry->orig_modifiable;
		entry->modified = false;
		entry->orig_value = NULL;
		entry->orig_modifiable = false;
	} ZEND_HASH_FOREACH_END();

	ctx->ini_values = values;
	ctx->ini_mods = EG(modified_ini_directives);
	EG(modified_ini_directives) = NULL;
}
/* }}} */

void fpm_coop_ini_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_ini_entry *entry;
	zend_string *name;
	zend_string *value;

	if (!ctx->ini_mods) {
		return;
	}

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(ctx->ini_mods, name, entry) {
		value = zend_hash_find_ptr(ctx->ini_values, name);

		/* Dokladnie to, co zend_alter_ini_entry_ex robi przy pierwszej
		 * modyfikacji w "rundzie": biezaca (bazowa) wartosc -> orig_value,
		 * wlasna wartosc requestu -> value. Transfer wskaznika, bez
		 * zend_string_copy/release. */
		entry->orig_value = entry->value;
		entry->orig_modifiable = entry->modifiable;
		entry->value = value;
		entry->modified = true;
	} ZEND_HASH_FOREACH_END();

	/* ini_values juz nie sa nam potrzebne — wartosci przeniesione wyzej,
	 * NULL-owy destruktor, wiec destroy nie zwalnia niczyich stringow. */
	zend_hash_destroy(ctx->ini_values);
	FREE_HASHTABLE(ctx->ini_values);
	ctx->ini_values = NULL;

	/* Tablica wraca jako EG(modified_ini_directives) — dalsze ini_set() w
	 * tym requescie i finalny zend_ini_deactivate() na koncu requestu
	 * (fpm_pool_coop.c) dzialaja jak gdyby nigdy nie zeszla z procesora. */
	EG(modified_ini_directives) = ctx->ini_mods;
	ctx->ini_mods = NULL;
}
/* }}} */

void fpm_coop_ini_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_string *value;

	/* Normalna sciezka tu nie wchodzi: request konczy sie NA PROCESORZE, wiec
	 * ostatni fpm_coop_ini_req_enter() juz oddal obie tablice, a
	 * zend_ini_deactivate() na koncu requestu przywrocil wpisy z on_modify.
	 * To jest zabezpieczenie na wypadek zniszczenia ctx requestu, ktory zszedl
	 * z procesora i nigdy nie wrocil (np. fiber ubity przy zamykaniu workera).
	 * Same wpisy ini sa juz wtedy w stanie bazowym — fpm_coop_ini_req_leave()
	 * przywrocil je przed zejsciem — wiec zostaje tylko zwolnic osierocone
	 * wartosci tego requestu. */
	if (ctx->ini_values) {
		ZEND_HASH_MAP_FOREACH_PTR(ctx->ini_values, value) {
			zend_string_release(value);
		} ZEND_HASH_FOREACH_END();
		zend_hash_destroy(ctx->ini_values);
		FREE_HASHTABLE(ctx->ini_values);
		ctx->ini_values = NULL;
	}
	if (ctx->ini_mods) {
		zend_hash_destroy(ctx->ini_mods);
		FREE_HASHTABLE(ctx->ini_mods);
		ctx->ini_mods = NULL;
	}
}
/* }}} */
