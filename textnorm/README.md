# textnorm — C port of ru_normalizr (in progress)

Plan: RESEARCH-text-normalization.md §10. Python ru_normalizr is the oracle; the C port must
match its output byte for byte in compat mode; bugs of the originals are fixed behind flags
and listed in `QUIRKS.md` for review.

## Layout
- `oracle/make-corpus.py` — builds the input corpus (`core.jsonl`: rn tests, normbench,
  norm-test, a fixed sample of the public-domain Wikisource books; `big.jsonl`: all book
  lines plus extra texts such as the 4pda topic, not committed).
- `oracle/trace.py` — runs ru_normalizr with `NormalizeOptions.tts()` and records the output
  and every pipeline stage (`steps`), so the port can be checked pass by pass.
- `tests/golden/core.jsonl.zst` — oracle output for `core.jsonl` (14 170 records,
  `{"id","in","out","steps"}`).
- `cmorphy3/` — pymorphy3 2.0.6 in C (`cmorphy3.h`, `cmorphy3.c`; "c" instead of "py"): parse, lexeme, inflect,
  word_is_known, tag categories; same parses, scores (bit for bit) and forms as pymorphy3 with
  the pure-Python DAWG reader.
  - `mkdict.py DICT_DIR cmorphy3-ru.bin` converts pymorphy3-dicts-ru to one mmapped file (16 MB).
  - `gen-unidata.py > unidata.h`: Unicode tables (lower(), isalpha, …) taken from the oracle's
    Python 3.12.3, so str methods behave the same.
  - Check: `cmorphy3-dump cmorphy3-ru.bin < words` and `pymorphy3-dump.py < words` (pymorphy3, run in
    claude-ru-normalizr), then `compare-dump.py ref.jsonl c.jsonl`: parses, lexemes of the first
    3 parses and 27 inflections each. 2026-11-21: 63 373 words (core corpus tokens + edge cases)
    and 238 631 more from big.jsonl
    identical; ASan/UBSan clean; 63k words with all lexemes/inflections in 8.5 s.
  - Known oracle nondeterminism: `tag.animacy` of the 4 Inmx tags carrying both anim and inan
    depends on PYTHONHASHSEED in Python; the C code takes the first in the tag string.

- `num2words/` — num2words 0.5.14 lang_RU in C (`n2w-ru.h`, `n2w-ru.c`; LGPL-2.1-or-later
  like num2words): cardinal and ordinal with case/gender/plural/animate and the float form
  ("две целых пять десятых"). `fixes = 0` gives num2words exactly; bugs found are listed
  in `QUIRKS.md` and fixed behind `N2W_FIX_*` flags.
  - `gen-tables.py > n2w-ru-tables.h`: num2words' word tables expanded to
    [num][case][gender|plural][animate] by its own case_classifier_element.
  - Check: `gen-numbers.py > numbers` (30 569 ints, negatives, floats), `pyn2w-dump.py` (in
    claude-ru-normalizr) and `n2w-dump` over 144 option combos each (`n2w-dump` without `--fixes`), then `cmp`.
    2026-11-21: 4 401 936 lines identical, ASan/UBSan clean; 6 s against 91 s in Python.

- `rn/` — the ru_normalizr passes (in progress). Regexes run on PCRE2 (libpcre2-32: UTF-32
  text, so offsets are Python's code point indices) with Python's semantics (`rn-re.h`:
  search/match/fullmatch, finditer with Python's empty-match rule).
  - `gen-patterns.py RN_SRC CORPUS OUT` (in claude-ru-normalizr) records every pattern rn
    compiles (import time and while normalizing the corpus), translates each from Python's
    parse tree to PCRE2 (single characters as the exact sets Python matches, IGNORECASE
    pre-expanded, `\w`/`\d`/`\s`/`\b`/`^`/`$` with Python's meaning) and writes
    `patterns.jsonl`, `rn-patterns.h` (enum + PCRE2 strings, named after rn's module
    variables) and `rn-unidata.h` (Python's `\w` and `\d` as ranges; `rn_re_compile()`
    puts them in place of `\p{L}\p{N}_` / `\p{Nd}`, so PCRE2's Unicode version does not
    matter: noble's 10.42 has Unicode 14, the oracle 15.0).
  - Check: `mk-parity-texts.py tests/golden/core.jsonl.zst texts.bin` (all distinct strings of
    the goldens, steps included, plus all code points in pieces of 2000), then `re-parity` (C) and
    `py-re-parity.py` (Python) print per pattern the match count and a digest of all group
    spans; `re-charsets py-charsets.txt` compares the classes code point by code point.
  - `rn-patterns.h`, `rn-unidata.h` are generated (committed so the C build needs no
    oracle). `rn-ph.[ch]` placeholders.py; passes (`rn-pass.h`): `rn-urls.c` run_urls,
    `rn-protect.c` protect_windows_paths / protect_overlong_numbers, `rn-math.c` run_math.
  - Check a pass: `oracle/stage-pairs.py GOLDEN STAGE pairs.bin` (STAGE such as run_math, or
    `run_urls>run_math` for the unrecorded gap between two stages), then
    `rn-stage-test STAGE pairs.bin`. Sub-steps inside a stage: `oracle/trace-sub.py` records
    (in, out) of single rn functions. 2026-11-21: run_urls, protect, run_math exact on core
    (14 157 pairs) and big (75 771).
  - Python str semantics: `gen-pyuni.py` (run in the oracle's Python) writes `rn-pyuni.h`;
    `rn-pystr.[ch]` isalpha/isdecimal/…/lower/upper/casefold/capitalize, str.replace/strip/find;
    `pystr-check.c` vs `pystr-check.py` agree on all 1 112 070 code points.
  - `rn-morph.[ch]`: one cmorphy3 handle for the passes (word_is_known); rn-stage-test
    opens it from `$RN_MORPH_DICT`.
  - `rn-pre.[ch]`: the preprocess helpers, one `rn_pre_<name>` per Python function, also
    stages of rn-stage-test under the Python name (checked on trace-sub pairs). 2026-11-21:
    the 51 functions in `RN_PRE_FUNCS` exact on core: `rn-pre.c` (cleanup helpers),
    `rn-numbering.c` (numbering.py), `rn-refs.c` (references.py), `rn-ident.c`
    (identifiers.py).
  - `rn-num.[ch]`: rn's num2words wrappers (cardinal/ordinal with its fallbacks, roman).
  - `rn-tables.[ch]`: rn's constant dicts/sets (letter names, numeric suffixes, …);
    `gen-tables.py RN_SRC rn-tables-data.h` (in claude-ru-normalizr) dumps them.
  - `rn-phone.[ch]`: the part of phonenumbers 9.0.43 that phones.py calls (parse of
    "+digits", is_valid_number, E164/INTERNATIONAL formatting); `gen-phonemeta.py OUT.h`
    dumps its metadata to `rn-phonemeta.h` (libphonenumber metadata, Apache-2.0).
    Check: `phone-check.py gen > numbers` (example numbers of every region, variants,
    random), `phone-check.py` vs `phone-check` (C): 143 648 inputs (22 153 valid) identical
    (2026-11-21). `rn-phones.c` phones.py, `rn-abbr.c` the numeric/era abbreviations,
    `rn-address.c` address.py, `rn-caps.c` caps.py (first-word and caps-line decapping).
  - `rn-stage.c`: `_run_preprocess_steps` (as normalize_text calls it, without caps),
    `_run_caps_normalization`, remove_numeric_footnotes (rn-stage-test stage names; the
    last is checked on the gap `_run_caps_normalization>run_dates_time`). 2026-11-21: all
    three exact on core (14 154 pairs) and big (75 763), ASan/UBSan clean.
  - `rn-mx.[ch]`: parse_word with a cache, grammeme indices, canonical cases (shared by the
    case rules and roman numerals); `rn-ncase.[ch]`: get_numeral_case with the 33 rules of
    numerals/_rules.py, simple_tokenize.
  - `rn-roman.c`: normalize_roman (13 steps), stage `run_roman`. 2026-11-21: exact on core
    (13 968 pairs) and big (75 757), ASan/UBSan clean. `RN_FIXES=0x1` in rn-stage-test turns
    on the fixes (`rn_fixes`, QUIRKS.md R5).
  - `rn-dates.c`: normalize_dates_and_time, stage `run_dates_time`. 2026-11-21: exact on core
    (13 814 pairs), big (75 743) and 34 probes of every branch; ASan/UBSan clean. Fixes
    R6–R11 (time reading, date case) are `RN_FIXES` 0x2–0x40.
  - `rn-years.c`: normalize_years + normalize_numeric_ranges, stage `run_years`.
    2026-11-21: exact on core (13 754 pairs), big (75 739) and 23 probes; ASan/UBSan clean.
    Fixes R12–R17 are `RN_FIXES` 0x100–0x2000.
  - `rn-so.c`: our own pass for R4 ("со сто восьмой", "со второго"), `RN_FIXES` 0x80;
    to run last. `RN_N2W_FIXES` in rn-stage-test sets the num2words fixes (N1–N4).
  - `rn-unitform.c`: resolve_unit_form (a unit by an inflected name; needs the morphology).
  - `rn-units.[ch]`: UnitRegistry.resolve and resolve_unit_expression; `gen-units.py RN_SRC
    rn-units-data.h` (in claude-ru-normalizr) dumps rn's unit tables. Check: `units-check.py`
    vs `units-check.c` over 95 026 corpus tokens, identical (2026-11-21).

## Oracle environment (goldens of 2026-11-21)
- ru_normalizr: our fork, `_tmp/ru-normalizr` HEAD cb5f247 (1.3.0 + agreement fixes).
- Python 3.12.3 (docker image claude-ru-normalizr on mikhail), pymorphy3 2.0.6,
  pymorphy3-dicts-ru 2.4.417150.4580142, DAWG2-Python 0.9.0 (the DAWG2 C extension is
  disabled in trace.py, as in the PPA), num2words 0.5.14.
- Runs: mikhail `~/claude-builds/tn` (`out/core.jsonl` 43 s, `out/big.jsonl` 75 771 records,
  342 s).
