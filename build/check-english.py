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
]

WORD_RE = re.compile(
    r"(?i)(?:[ąćęłńóśźż]|\b(?:" + "|".join(map(re.escape, WORDS)) + r")\b)"
)

findings = []


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
    ap.add_argument("paths", nargs="*", help="optional subset of files to check")
    args = ap.parse_args()

    if args.commits:
        check_commits(args.commits)

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
