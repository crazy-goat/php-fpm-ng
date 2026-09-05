# patches/

Łatki na pliki php-src **poza `sapi/`**. Normalnie fpm-ng nie tyka upstreamu —
`prepare.sh` tworzy wyłącznie `sapi/fpmng/`. Te łatki są odstępstwem i mają być
widoczne, policzalne i tymczasowe.

## Zasady

1. **Każda łatka ma termin ważności.** W nagłówku podaje PR upstreamu, na który
   czeka. Znika w dniu, w którym tamten się zmerguje. Łatka bez drogi wyjścia
   to ukryty fork.
2. **Jedna łatka na jeden problem**, nie na jedną wersję PHP. Wersje obsługuje
   się dopiero wtedy, gdy ta sama łatka przestaje się nakładać.
3. **Więcej niż dwa warianty wersyjne jednej łatki to sygnał alarmowy** — wtedy
   albo idzie do upstreamu, albo trzeba ją przeprojektować tak, żeby zmieściła
   się w `sapi/fpmng/`.
4. **CI buduje każdą wspieraną wersję PHP**, więc nienakładająca się łatka pada
   przy budowaniu, a nie przy wydaniu.

## Układ

```
patches/*.patch              nakładane zawsze
patches/php-8.4/*.patch      tylko dla tej wersji, nadpisuje łatkę o tej nazwie
```

## Stan

| łatka | dotyczy | czeka na | sprawdzone na |
|---|---|---|---|
| `0001-gh18956-fastcgi-keepalive-counting.patch` | `main/fastcgi.c`, `main/fastcgi.h` | https://github.com/bukka/php-src/pull/2 (GH-18956) | 8.3, 8.4, 8.5, master |

### Dlaczego 0001 jest konieczna

Bramka trzyma trwałe połączenia do poola (`FCGI_KEEP_CONN`), więc fpm-ng jest
dokładnie tym przypadkiem, który błąd GH-18956 psuje: licznik idle kontra active
kłamie, a `pm = dynamic` i `ondemand` źle skalują pulę. Bez tej łatki wiarygodny
jest tylko `pm = static`.

Informacja o tym, czy `accept` przyszedł z połączenia trzymanego przy życiu,
żyje w `main/fastcgi.c` — nie da się jej wyprodukować z samego `sapi/`.
Towarzyszące zmiany w `fpm_request.c` i `fpm_request.h` niesiemy jako własne
pliki w `sapi/fpmng/fpm/`, bo te są w `sapi/`.
