# Znane problemy executora Fiber

Stan na 2026-09-06. `pool.executor = fiber` jest eksperymentalny i nie jest przeznaczony do użycia produkcyjnego.

## Zakres

Problemy dotyczą konfiguracji:

```ini
pool.type = fastcgi-ng | http
pool.executor = fiber
```

Nie dotyczą produkcyjnych ścieżek `fastcgi`, `fastcgi-ng/classic` ani `http/classic`.

Fiber wymaga wyłączonego OPcache:

```ini
php_admin_flag[opcache.enable] = off
```

## `max_execution_time` nie przerywa requestu

Request wykonujący nieskończoną pętlę nie jest przerywany po przekroczeniu skonfigurowanego limitu:

```php
<?php while (true) {}
```

Przykładowa konfiguracja:

```ini
php_admin_value[max_execution_time] = 1
```

Klient nadal czeka po upływie jednej sekundy. Zachowanie potwierdzono również na binarce sprzed optymalizacji trwałych handlerów sygnałów, więc nie jest to regresja tej optymalizacji.

### Przyczyna

Executor Fiber nie wykonuje pełnego `php_request_startup()` i `php_request_shutdown()` osobno dla każdego requestu. Timeout Zend jest procesowy (`setitimer()` i `SIGPROF`), natomiast jeden proces może obsługiwać wiele fiberów. Jeden procesowy timer nie reprezentuje niezależnych deadline'ów wielu requestów.

### Możliwa naprawa

Pełna implementacja wymaga:

1. deadline'u przechowywanego osobno dla każdego fibera;
2. kolejki timerów zintegrowanej z schedulerem;
3. przełączania aktywnego timera przy suspend/resume;
4. przypisania `SIGPROF` do aktualnie wykonywanego fibera;
5. bezpiecznego przerwania tylko jednego requestu;
6. obsługi kodu CPU-bound, który nie wraca do event loop;
7. potwierdzenia, że bailout nie uszkadza współdzielonego stanu procesu.

Do czasu implementacji konfiguracja Fiber powinna jawnie odrzucać niezerowe `max_execution_time`, zamiast przyjmować ustawienie, którego nie egzekwuje.

## Handlery `pcntl_signal()` przeciekają między requestami

Handler ustawiony w jednym requeście pozostaje widoczny w następnym requeście obsługiwanym przez ten sam proces:

```php
// Request 1
pcntl_signal(SIGUSR1, static function (): void {});

// Request 2
var_dump(pcntl_signal_get_handler(SIGUSR1) === SIG_DFL); // false
```

W teście otrzymano:

```text
SET -> LEAK
```

Dla executora `classic` ten sam test daje:

```text
SET -> RESET
```

### Przyczyna

Fiber nie wykonuje pełnego request shutdown, który w klasycznej ścieżce resetuje logiczną tablicę handlerów Zend. Handlery sygnałów są stanem procesowym, a nie naturalnie lokalnym dla fibera.

### Możliwa naprawa

Stan handlerów Zend trzeba dołączyć do kontekstu requestu Fiber:

1. zapisywać go przy zawieszeniu fibera;
2. przywracać przed wznowieniem;
3. resetować po zakończeniu requestu;
4. zachować wewnętrzne handlery wymagane przez Zend i FPM;
5. przetestować kilka współbieżnych requestów ustawiających różne handlery.

Do czasu implementacji należy uznać `pcntl_signal()` i inne operacje zmieniające procesowe handlery za nieizolowane w executorze Fiber. Można rozważyć ich jawne blokowanie w tym trybie.

## `pcntl_fork()` wewnątrz requestu

Samo wywołanie `exit()` w potomku utworzonym przez `pcntl_fork()` nie musi zakończyć procesu FPM. Kończy wykonywanie skryptu, po czym potomny proces może przejść do dalszej części pętli workera. Rodzic oczekujący przez `pcntl_waitpid()` może wtedy blokować się bez końca.

Test procesu potomnego musi kończyć go na poziomie systemowym, na przykład kontrolowanym sygnałem, i sprawdzać status przez `pcntl_wifsignaled()`.

Kontrolowany test wykazał, że po zakończeniu potomka przez `SIGKILL`:

- rodzic poprawnie odbiera status przez `waitpid()`;
- worker obsługuje następny request;
- reload wymienia worker;
- zatrzymanie procesu działa poprawnie.

Mimo tego `pcntl_fork()` należy traktować jako nieobsługiwane API aplikacyjne w executorze Fiber, ponieważ duplikuje cały wielorequestowy proces wraz z schedulerem, deskryptorami i współdzielonym stanem.

## Potwierdzone działające elementy

Dla `fastcgi-ng/fiber` i `http/fiber` na czystym release buildzie PHP 8.5 potwierdzono:

- małą i dużą odpowiedź;
- binarny POST;
- FastCGI/HTTP keep-alive i `Connection: close`;
- przeżycie zerwania połączenia przez klienta;
- kolejny request po błędzie klienta;
- kontrolowany fork zakończony sygnałem;
- reload przez `SIGUSR2`;
- zatrzymanie przez `SIGTERM`;
- wymóg wyłączonego OPcache.

## Rekomendacja

Nie rozszerzać bieżącej poprawki release o przebudowę lifecycle Fiber. Kolejność dalszych prac:

1. jawnie odrzucić niezerowe `max_execution_time` dla Fiber;
2. udokumentować lub zablokować procesowe API `pcntl`;
3. na osobnym branchu zaimplementować izolację handlerów sygnałów;
4. per-fiber timeout potraktować jako osobny projekt wymagający testów współbieżności, bailoutów i kodu CPU-bound;
5. utrzymać oznaczenie Fiber jako eksperymentalnego do czasu rozwiązania tych problemów.
