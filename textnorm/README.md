# textnorm — C port of ru_normalizr (in progress)

Plan: RESEARCH-text-normalization.md §10. Python ru_normalizr is the oracle; the C port must
match its output byte for byte, except for a reviewed list of deliberate fixes.

## Layout
- `oracle/make-corpus.py` — builds the input corpus (`core.jsonl`: rn tests, normbench,
  norm-test, a fixed sample of the public-domain Wikisource books; `big.jsonl`: all book
  lines plus extra texts such as the 4pda topic, not committed).
- `oracle/trace.py` — runs ru_normalizr with `NormalizeOptions.tts()` and records the output
  and every pipeline stage (`steps`), so the port can be checked pass by pass.
- `tests/golden/core.jsonl.zst` — oracle output for `core.jsonl` (14 170 records,
  `{"id","in","out","steps"}`).
- `morph/` — pymorphy3 2.0.6 in C (`morph.h`, `morph.c`): parse, lexeme, inflect,
  word_is_known, tag categories; same parses, scores (bit for bit) and forms as pymorphy3 with
  the pure-Python DAWG reader.
  - `mkmorph.py DICT_DIR ru-morph.bin` converts pymorphy3-dicts-ru to one mmapped file (16 MB).
  - `gen-unidata.py > unidata.h`: Unicode tables (lower(), isalpha, …) taken from the oracle's
    Python 3.12.3, so str methods behave the same.
  - Check: `morph-dump ru-morph.bin < words` and `morph-dump.py < words` (pymorphy3, run in
    claude-ru-normalizr), then `compare-dump.py ref.jsonl c.jsonl`: parses, lexemes of the first
    3 parses and 27 inflections each. 2026-11-21: 63 373 words (core corpus tokens + edge cases)
    identical; ASan/UBSan clean; 63k words with all lexemes/inflections in 8.5 s.
  - Known oracle nondeterminism: `tag.animacy` of the 4 Inmx tags carrying both anim and inan
    depends on PYTHONHASHSEED in Python; the C code takes the first in the tag string.

## Oracle environment (goldens of 2026-11-21)
- ru_normalizr: our fork, `_tmp/ru-normalizr` HEAD cb5f247 (1.3.0 + agreement fixes).
- Python 3.12.3 (docker image claude-ru-normalizr on mikhail), pymorphy3 2.0.6,
  pymorphy3-dicts-ru 2.4.417150.4580142, DAWG2-Python 0.9.0 (the DAWG2 C extension is
  disabled in trace.py, as in the PPA), num2words 0.5.14.
- Runs: mikhail `~/claude-builds/tn` (`out/core.jsonl` 43 s, `out/big.jsonl` 75 771 records,
  342 s).
