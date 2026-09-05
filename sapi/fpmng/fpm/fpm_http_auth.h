/* fpm-ng: AUTH_TYPE / REMOTE_USER dla bramki HTTP (fpm_http.c).
 *
 * Wyprowadzone z naglowka Authorization, dokladnie tak jak kazdy serwer
 * CGI/FastCGI by to zrobil. Bramka SAMA niczego nie uwierzytelnia -- tylko
 * przekazuje do aplikacji to, co przyszlo w naglowku:
 *   - "Basic <base64>"  -> AUTH_TYPE=Basic, REMOTE_USER = user z "user:pass"
 *   - kazdy inny schemat -> tylko AUTH_TYPE, REMOTE_USER nie jest ustawiane
 *
 * Wlasny dekoder base64: gateway to osobny proces bez zainicjowanego Zend MM
 * (patrz docs/NOTES.md, sekcja o typach poola), wiec php_base64_decode_ex()
 * z ext/standard nie jest tu bezpieczne do wywolania.
 */

#ifndef FPM_HTTP_AUTH_H
#define FPM_HTTP_AUTH_H 1

#define FPM_HTTP_AUTH_TYPE_LEN 32
#define FPM_HTTP_AUTH_USER_LEN 256

/* authorization_header moze byc NULL (brak naglowka) -- wtedy oba bufory
 * wychodza puste. Oba bufory sa zawsze zero-terminowane; pusty [0] == '\0'
 * oznacza "nie ustawiaj tego parametru CGI". */
void fpm_http_auth_parse(const char *authorization_header,
	char auth_type[FPM_HTTP_AUTH_TYPE_LEN], char remote_user[FPM_HTTP_AUTH_USER_LEN]);

#endif
