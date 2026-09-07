#!/usr/bin/env python3
"""Deterministic English-only check for repository-owned files.

Scope (owned): sapi/fpmng/, ext/fpmng_metrics/, docs/, tasks/, README.md,
build/*.sh, .github/workflows/*.  Excluded: patch payloads under patches/
(files we do not own) and anything listed in build/english-allow.txt
(literal file:line prefixes for quoted material that must stay verbatim).

Detection:
  - C/H files: comment text only (/* */ and //). String literals are skipped,
    so error strings and log lines that must stay verbatim never fire.
  - Markdown: prose only. Fenced code blocks and inline code are skipped.
  - Shell: comment lines (#) outside quotes are approximated by scanning
    full-line comments only.
  - Polish with diacritics (any character from the set) or a Polish
    diacritic-less stopword lexicon (zeby, ktore, wiec, ...).

Commit messages: pass --commits BASE..HEAD to check git log subjects.

Exit code 0 = clean, 1 = findings.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

OWNED_DIRS = ["sapi/fpmng", "ext/fpmng_metrics", "docs", "tasks"]
OWNED_FILES = ["README.md"]
OWNED_GLOBS = ["build/*.sh", ".github/workflows/*.yml", "sapi/fpmng/config.m4"]
EXCLUDED_DIRS = ["patches"]

ALLOW_FILE = REPO / "build" / "english-allow.txt"

DIACRITICS = re.compile(r"[ąćęłńóśźżĄĆĘŁŃÓŚŹŻ]")

# Diacritic-less Polish words that never appear in technical English prose.
# Deliberately conservative; extend only after checking false positives on
# the real tree (acceptance criterion in tasks/011).
WORDS = [
    "zeby", "ktore", "ktory", "ktora", "ktorych", "ktorej", "ktorym",
    "ktos", "czegos", "czesc", "czym", "jakos", "kiedys",
    "wiec", "czyli", "czyz", "zaden", "zadna", "zadne", "zadnego", "zadnym",
    "niech", "oraz",
    "musi", "moze", "mozna", "mozliwe", "mozliwy", "nalezy",
    "zawsze", "nigdy", "wtedy", "potem", "znowu",
    "wlasnie", "wlasny", "wlasna", "wlasne",
    "wspolny", "wspolna", "wspolne", "wspolnie",
    "wszystko", "wszystkie", "wszystkim",
    "takze", "przynajmniej",
    "zrodlo", "zrodla", "zrodel",
    "wartosc", "wartosci", "wartoscia",
    "wyjscie", "wejscie", "powrot", "poczatek", "koniec", "srodek",
    "blad", "bledy", "bledu", "bledow",
    "dziala", "dzialac", "dzialanie", "dzialajacy",
    "wywolac", "wywoluje", "wywolania", "wolane", "wolany", "wolania",
    "pamiec", "pamieci", "pamiecia",
    "polaczenie", "polaczen", "polaczenia",
    "zadanie", "zadania", "zawartosc", "zawartosci",
    "wykonuje", "wykonac", "wykonania", "wykonanie",
    "zwolnic", "zwolnienie",
    "przejscie", "przejscia", "przechodzi", "przechowywany", "przechowywac",
    "odczyt", "odczytu", "zapisu", "zapisane", "zapisywany",
    "aktualny", "aktualnie", "biezacy", "biezace", "biezacego", "biezacych",
    "wyjatek", "wyjatkiem", "wyjatki",
    "osobny", "osobne", "osobno", "swiezy", "swieza", "swieze",
    "zycie", "pusty", "pusta", "puste",
    "swoj", "swoja", "swojego", "swoim", "nasz", "nasze", "nasza",
    "zamiast", "ponizej", "powodu", "przypadku", "przypadek",
    "przyklad", "przykladowo",
    "ustawia", "ustawic", "ustawienia", "sprawdzic", "sprawdzenie",
    "zaleznosc", "zaleznosci", "procesowy", "procesu", "procesie",
    "watek", "watki", "watku", "tablica", "tablice", "tablicy",
    "wskaznik", "wskazniki", "wskaznika",
    "przebieg", "przebiegu", "przebiegow", "procesy",
    "uruchamia", "uruchomienie", "uruchomiony", "uruchomic",
    "tworzenie", "tworzony", "usuwanie", "usuwany",
    "czyta", "czytanie", "czytany", "trzyma", "trzymac", "trzymany",
    "niczego", "czekamy", "czekanie", "czekac",
    "zdarzenie", "zdarzen", "zdarzenia", "petli", "petla",
    "kolejny", "kolejne", "kolejna", "kolejnym", "kolejnych",
    "wlacza", "wlaczone", "wlaczenia", "wlaczyc",
    "wylacza", "wylaczone", "wylaczenia", "wylaczyc", "wylaczony",
    "obsluguje", "obsluga", "obslugiwany",
    "wylacznie", "glownie", "glowny", "glowna", "glowne",
    "jedyny", "jedyna", "jednym",
    "dobrze", "lepiej", "najlepiej",
    "dokladnie", "rozne", "roznych", "inny", "inne", "innych", "innym",
    "bedzie", "beda", "jeszcze", "najpierw", "znaczenie", "znaczaca",
    "poprawka", "poprawke", "roboty", "pisac", "pisze", "komu",
    "niepotrzebny", "zwraca", "zwraca", "zwracaja", "zaladowany",
    "zakonczyl", "zakonczenia", "dokonczenie", "metryk", "metryki",
    "metryka", "celu", "samo", "sam", "sama", "przegapilem", "przegapione",
    "wskaznikiem", "dopasowuje", "przypisano", "oszczedzamy", "odbiorcy",
    "znacznik", "nastepnym", "wtedy", "takim", "takie", "takich",
]

WORD_RE = re.compile(
    r"(?i)(?:[ąćęłńóśźż]|\b(?:" + "|".join(map(re.escape, WORDS)) + r")\b)"
)

# English dictionary mode: any alphabetic token (>= 4 chars) in a comment or
# in prose that is not in the dictionary and not in the technical allowlist
# below is a finding. This is the airtight guarantee — a diacritic-less
# Polish word cannot pass, because no Polish word is an English word.
# Dictionary sources, first existing wins; CI installs `wamerican`.
DICT_PATHS = [
    "/usr/share/dict/words",
    "/usr/share/dict/american-english",
    "/usr/share/dict/british-english",
]

def load_dict():
    import os
    for p in DICT_PATHS:
        if os.path.exists(p):
            words = set()
            with open(p, encoding="utf-8", errors="replace") as fh:
                for ln in fh:
                    w = ln.strip().lower()
                    if w:
                        words.add(w)
                        # dictionaries list possessives like "abc's"
                        if w.endswith("'s"):
                            words.add(w[:-2])
            return words
    return None

# Technical vocabulary that is not in an English dictionary but is legitimate
# in this tree's comments (identifiers, API names, Unix/PHP/HTTP terms).
TECHNICAL = set("""
    api argv ascii asan aws backoff bit backlog bash bigint bool breakage
    builtin byte cacheline ccache cgroup cgi checksum clang clang-tidy cli
    cmake coredump coredumps cpp cpu cpus cron crontab csl css ctype curl
    datagram datagrams debug delta dev devpoll diff dma dns docker docroot
    domain dot dso dst dtrace dyn dynlib efree eg endpoints enum env errno
    esc exe exit ext fallback fastcgi fcgi fd fds fiber fibers fifo flag
    flags fork fossil freebsd front fsync ftp gc gcc gdb gettext git glob
    globals goto grpc glibc gui hash hashtable headerfilter hex histogram
    histograms http https idempotent idx idn ilp32 impl ini inotify int
    internals io ip ipv4 ipv6 iso ist iterator jemalloc jit json juggling
    kill kv lang latency latin ldap libc libevent libev libmagic libtool
    libxml linux localhost lockfile lookahead lpc lseek macro mainline
    malloc mbstring mem memcache metadata metric metrics middleware mingw
    mlock mmap ms msan mutex mysql nd thrift namespace nan ndelay net
    newline nginx nls nts null opcode opcache opcodes osi output ping
    pathname pid pids pidfd pidfds pkg platform plausibly posix post pp
    pragma printf prio proto proxy pthread pthreads ptr ptrace rcache
    re2c readline redis refcount refcounted refcounting regex reinit
    revalidate revalidates rfc rlimit rpc rusage runtime scalar scoreboard
    sched scripts sdk serialize session sessions shm shutdown sigaction
    siginfo sigkill signal signals sigpipe sigprocmask sigquit sigsegv
    sigterm sigusr sigwait skeleton sleep slop smtp socket sockets
    sockaddr solver spawn spawned spec sprintf ssql ssrc ssl sso stat
    static stdlib stopwatch str stream strncmp struct stub subdir
    subfolders subsecond symlink sync sys syscall syscalls sysconf sysdir
    sysv tcp tcpdump template tid timeout timeouts tls tmp tmpdir todo
    token tokens trace tracer traffic tsrm ttl tty txn typedef udp uds
    uid uio unix unlock unlogged upload uptime uri url urls utf utf8
    validator var verbose vfs vhost vhosts watchable watchdog webdav
    whitespace winbind ws xdebug xhtml xml xsendfile yaml zend zendiag
    zlog zts aarch64 alpn sni oniguruma apparmor acl uds cve dns
    acceptors acceptor arginfo backported boolen bufferevent bufferevents
    capath chdir chown config cwc dlopen dns dsos eio endianness
    environment eventbase evutil expat fastcgi's fcg fd's fpm's
    freelists glibc's hashtable's http's ini's lowercase malloc'ed
    mlock'ed nproc php's pidfd's pollfd pollset ppid preforked
    preforking realloc ruid sandboxing sockaddr's spawnable ssl's
    strlcpy subclassed tcp's timezone's uds's uid's umask uname
    uninitialized url's utf's zval zvals zpp zend's zts's
""".split())

TOKEN_RE = re.compile(r"[A-Za-z][A-Za-z'-]{2,}")

def dict_check(text, path, lineno, why):
    """Return finding lines for words unknown to the English dictionary."""
    hits = []
    dictionary = DICT if DICT is not None else set()
    for m in TOKEN_RE.finditer(text):
        tok = m.group(0)
        # skip mixed-case identifiers (camelCase, URLs fragments, macros)
        stripped = tok.strip("'")
        if any(c.isupper() for c in stripped[1:]) and not stripped.isupper():
            continue
        low = stripped.lower().replace("-", "")
        if low in DICT or low in TECHNICAL:
            continue
        if len(low) < 4:
            continue
        hits.append((path, lineno, text, f"{why}: unknown word '{tok}'"))
    return hits

findings = []
DICT = None  # populated in main() when --dict is enabled
POLISH = None  # populated in main() from the vendored Polish dictionary
ENGLISH = None  # populated in main() from the vendored English dictionary

# Polish dictionary mode (authoritative): any token found in the vendored
# Polish frequency dictionary (top 50k OpenSubtitles forms, source:
# https://github.com/hermitdave/FrequencyWords, content/2018/pl) is Polish.
# Both the dictionary and the tokens are diacritic-folded before lookup, so
# Polish written without diacritics ("zeby", "ktore") is caught too.
POLISH_DICT_FILE = REPO / "build" / "polish-dict.txt"
FOLD = str.maketrans("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ", "acelnoszzACELNOSZZ")

# Words that are genuine Polish dictionary words but also occur as normal
# English words or well-known technical terms in this tree.
POLISH_IGNORE = {
    "data", "para", "waga", "wata", "kasa", "masa", "rada", "wada",
    "kara", "mala", "baza", "dana", "dane", "moment", "operator", "karta",
    "port", "porty", "testy", "test", "info", "motyw", "notka", "stan",
    "stanu", "pasta", "maska", "taski", "lista", "listy", "plik", "pliki",
    "edytor", "edytory", "format", "formatu", "kopia", "kopie", "mega",
    "stop", "start", "status", "limit", "limity", "cache", "klon", "kod",
    "kodu", "kompilacja", "rezultat", "rezultaty", "tryb", "trybu",
}

ENGLISH_DICT_FILE = REPO / "build" / "english-dict.txt"

def load_english():
    if not ENGLISH_DICT_FILE.exists():
        return None
    words = set()
    for ln in ENGLISH_DICT_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        w = ln.strip().lower()
        if w:
            words.add(w)
    return words

def load_polish():
    if not POLISH_DICT_FILE.exists():
        return None
    words = set()
    for ln in POLISH_DICT_FILE.read_text(encoding="utf-8").splitlines():
        w = ln.split()[0].translate(FOLD).lower()
        if len(w) >= 3:
            words.add(w)
    return words

def polish_check(text, path, lineno, why):
    """Flag tokens that are Polish dictionary words, or that are not English
    dictionary words (combined rule: Polish OR not-English)."""
    hits = []
    if POLISH is None and ENGLISH is None:
        return hits
    for m in TOKEN_RE.finditer(text):
        tok = m.group(0)
        stripped = tok.strip("'")
        # skip mixed-case identifiers (camelCase, macros), keep ALL-CAPS
        if any(c.isupper() for c in stripped[1:]) and not stripped.isupper():
            continue
        low = stripped.lower().translate(FOLD)
        if len(low) < 3 or low in POLISH_IGNORE:
            continue
        if POLISH is not None and low in POLISH:
            hits.append((path, lineno, text, f"{why}: Polish word '{tok}'"))
            continue
        if len(low) >= 4 and ENGLISH is not None and low not in ENGLISH and low not in TECHNICAL:
            hits.append((path, lineno, text, f"{why}: not an English word '{tok}'"))
    return hits


def add(path, lineno, text, why):
    findings.append(f"{path}:{lineno}: {why}: {text.strip()[:120]}")


def allowed(path, lineno):
    try:
        prefixes = ALLOW_FILE.read_text().splitlines()
    except FileNotFoundError:
        return False
    rel = str(path.relative_to(REPO))
    needle = f"{rel}:{lineno}:"
    return any(p.strip() == needle for p in prefixes)


def iter_owned():
    for d in OWNED_DIRS:
        base = REPO / d
        if base.is_dir():
            for p in base.rglob("*"):
                if p.is_file() and not p.is_symlink():
                    if any(str(p.relative_to(REPO)).startswith(e + "/") for e in EXCLUDED_DIRS):
                        continue
                    yield p
    for f in OWNED_FILES:
        p = REPO / f
        if p.is_file():
            yield p
    for g in OWNED_GLOBS:
        for p in REPO.glob(g):
            if p.is_file():
                yield p


def check_c(p):
    s = p.read_text(errors="replace")
    n = len(s)
    i = 0
    line = 1
    while i < n:
        c = s[i]
        nxt = s[i + 1] if i + 1 < n else ""
        if c == '"':
            i += 1
            while i < n and s[i] != '"':
                i += 2 if s[i] == "\\" else 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n and s[i] != "'":
                i += 2 if s[i] == "\\" else 1
            i += 1
            continue
        if c == "/" and nxt == "/":
            j = i
            while j < n and s[j] != "\n":
                j += 1
            seg = s[i:j]
            if WORD_RE.search(seg):
                add(p, line, seg, "Polish in // comment")
            if DICT:
                for f in dict_check(seg, p, line, "dictionary"):
                    add(*f)
            if POLISH:
                for f in polish_check(seg, p, line, "polish-dict"):
                    add(*f)
            line += seg.count("\n")
            i = j
            continue
        if c == "/" and nxt == "*":
            j = i + 2
            while j + 1 < n and not (s[j] == "*" and s[j + 1] == "/"):
                j += 1
            j = min(n, j + 2)
            seg = s[i:j]
            if WORD_RE.search(seg):
                start = line
                for k, ln in enumerate(seg.splitlines(), start):
                    if WORD_RE.search(ln):
                        add(p, k, ln, "Polish in /* */ comment")
            if DICT:
                for k, ln2 in enumerate(seg.splitlines(), line):
                    for f in dict_check(ln2, p, k, "dictionary"):
                        add(*f)
            if POLISH:
                for k, ln2 in enumerate(seg.splitlines(), line):
                    for f in polish_check(ln2, p, k, "polish-dict"):
                        add(*f)
            line += seg.count("\n")
            i = j
            continue
        if c == "\n":
            line += 1
        i += 1


FENCE = re.compile(r"^(```|~~~)")


def check_md(p):
    s = p.read_text(errors="replace")
    in_fence = False
    for lineno, ln in enumerate(s.splitlines(), 1):
        if FENCE.match(ln.strip()):
            in_fence = not in_fence
            continue
        if in_fence:
            # Inside a fence, only diacritics are checked: the word lexicon
            # would fire on quoted program output, but Polish diacritics in
            # code/output are never legitimate here.
            if DIACRITICS.search(ln):
                add(p, lineno, ln, "Polish diacritics in code block")
            continue
        # strip links but NOT inline code (Polish diacritics hide there)
        prose = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", ln)
        no_inline = re.sub(r"`[^`]*`", " ", prose)
        if WORD_RE.search(no_inline) or DIACRITICS.search(prose):
            add(p, lineno, ln, "Polish in markdown prose")
        if DICT and not in_fence:
            for f in dict_check(no_inline, p, lineno, "dictionary"):
                add(*f)
        if POLISH and not in_fence:
            for f in polish_check(no_inline, p, lineno, "polish-dict"):
                add(*f)


def check_sh(p):
    for lineno, ln in enumerate(p.read_text(errors="replace").splitlines(), 1):
        stripped = ln.strip()
        if stripped.startswith("#") and WORD_RE.search(stripped):
            add(p, lineno, ln, "Polish in shell comment")


def check_m4(p):
    for lineno, ln in enumerate(p.read_text(errors="replace").splitlines(), 1):
        stripped = ln.strip()
        if stripped.startswith("dnl") and WORD_RE.search(stripped):
            add(p, lineno, ln, "Polish in m4 comment")


def check_yaml(p):
    for lineno, ln in enumerate(p.read_text(errors="replace").splitlines(), 1):
        stripped = ln.strip()
        if stripped.startswith("#") and WORD_RE.search(stripped):
            add(p, lineno, ln, "Polish in yaml comment")


CHECKERS = {
    ".c": check_c, ".h": check_c,
    ".md": check_md,
    ".sh": check_sh,
    ".m4": check_m4,
    ".yml": check_yaml, ".yaml": check_yaml,
}


def check_commits(spec):
    out = subprocess.run(
        ["git", "log", "--format=%H %s%n%b", spec],
        capture_output=True, text=True, check=True,
    ).stdout
    for lineno, ln in enumerate(out.splitlines(), 1):
        if WORD_RE.search(ln) or DIACRITICS.search(ln):
            print(f"commit-message (line {lineno}): Polish: {ln.strip()[:120]}")
            findings.append(f"<commits>:{lineno}: Polish in commit message")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--commits", help="git rev range (BASE..HEAD) whose commit messages are checked too")
    ap.add_argument("--dict", action="store_true", help="also flag comment/prose words missing from the English dictionary (strongest mode; requires /usr/share/dict/words or DICT_PATHS)")
    ap.add_argument("paths", nargs="*", help="optional subset of files to check")
    args = ap.parse_args()

    if args.commits:
        check_commits(args.commits)

    global DICT, POLISH, ENGLISH
    POLISH = load_polish()
    if POLISH is None:
        print("warning: build/polish-dict.txt missing; polish-dict mode off", file=sys.stderr)
    ENGLISH = load_english()
    if ENGLISH is None:
        print("warning: build/english-dict.txt missing; not-English mode off", file=sys.stderr)
    if args.dict:
        DICT = load_dict()
        if DICT is None:
            print("warning: no English dictionary found; --dict has no effect", file=sys.stderr)

    targets = [Path(p) for p in args.paths] if args.paths else list(iter_owned())
    for p in targets:
        fn = CHECKERS.get(p.suffix)
        if fn:
            fn(p)

    for f in findings:
        print(f)
    print(f"\n{len(findings)} finding(s)" + (" — FAIL" if findings else " — clean"))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
