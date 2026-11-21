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
- `morph/` — pymorphy3 in C (reads the pymorphy3-dicts-ru data converted to one mmapped file).

## Oracle environment (goldens of 2026-11-21)
- ru_normalizr: our fork, `_tmp/ru-normalizr` HEAD cb5f247 (1.3.0 + agreement fixes).
- Python 3.12.3 (docker image claude-ru-normalizr on mikhail), pymorphy3 2.0.6,
  pymorphy3-dicts-ru 2.4.417150.4580142, DAWG2-Python 0.9.0 (the DAWG2 C extension is
  disabled in trace.py, as in the PPA), num2words 0.5.14.
- Runs: mikhail `~/claude-builds/tn` (`out/core.jsonl` 43 s, `out/big.jsonl` 75 771 records,
  342 s).
