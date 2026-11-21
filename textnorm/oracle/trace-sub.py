#!/usr/bin/python3 -I
"""trace-sub.py RN_SRC CORPUS.jsonl OUT_DIR MODULE:FUNC...: (input, output) pairs of single
functions inside the pipeline, for checking a C pass piece by piece.

Every MODULE:FUNC (e.g. normalizer:normalize_linebreaks, preprocess_utils:clean_numbers)
is wrapped wherever ru_normalizr modules have imported it, and each call whose first
argument is a str is recorded (distinct inputs only). OUT_DIR/<func>.bin holds records as in
stage-pairs.py; keyword arguments other than the defaults of NormalizeOptions.tts() are
not recorded, so only functions called with one fixed set of arguments fit."""
import functools
import importlib
import json
import os
import pkgutil
import sys

sys.modules["dawg"] = None
sys.path.insert(0, sys.argv[1])
import ru_normalizr  # noqa: E402
from ru_normalizr import NormalizeOptions, Normalizer  # noqa: E402

mods = [importlib.import_module(m.name)
        for m in pkgutil.walk_packages(ru_normalizr.__path__, "ru_normalizr.")]
PAIRS = {}


def wrap(name, fn):
    @functools.wraps(fn)
    def w(text, *a, **kw):
        out = fn(text, *a, **kw)
        if isinstance(text, str) and isinstance(out, str):
            PAIRS[name].setdefault(text, out)
        return out
    return w


for spec in sys.argv[4:]:
    mod, func = spec.split(":")
    orig = getattr(importlib.import_module("ru_normalizr." + mod), func)
    PAIRS[func] = {}
    w = wrap(func, orig)
    for m in mods:
        for k, v in list(vars(m).items()):
            if v is orig:
                setattr(m, k, w)

norm = Normalizer(NormalizeOptions.tts())
with open(sys.argv[2], encoding="utf-8") as f:
    for line in f:
        try:
            norm.normalize(json.loads(line)["text"])
        except Exception:
            pass
os.makedirs(sys.argv[3], exist_ok=True)
for func, pairs in PAIRS.items():
    with open(os.path.join(sys.argv[3], func + ".bin"), "wb") as f:
        for i, o in pairs.items():
            for s in (i, o):
                b = s.encode("utf-8")
                f.write(b"%d\n" % len(b) + b)
    print("%s: %d pairs, %d changed" % (func, len(pairs), sum(i != o for i, o in pairs.items())),
          file=sys.stderr)
