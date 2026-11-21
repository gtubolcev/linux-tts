"""units-check.py RN_SRC CORPUS.jsonl TOKENS.txt OUT.txt: writes test tokens (corpus words,
all aliases and their case/dot variants) and UNIT_REGISTRY.resolve of each; units-check.c
prints the same for rn_unit_resolve."""
import json
import re
import sys

sys.path.insert(0, sys.argv[1])
from ru_normalizr.numerals._constants import UNIT_REGISTRY as R  # noqa: E402

toks = set()
for a in (*R._exact, *R._folded):
    for v in (a, a.upper(), a.lower(), a.title(), a.casefold(), a + ".", a + "..", " " + a,
              a.rstrip(".")):
        toks.add(v)
with open(sys.argv[2]) as f:
    for line in f:
        text = json.loads(line).get("text", "")
        for w in text.split():
            toks.add(w)
            toks.add(re.sub(r"^\W+|[^\w.]+$", "", w))
toks = sorted(t for t in toks if "\n" not in t and "\r" not in t and t == t.rstrip("\x85\x1c\x1d\x1e\x1f\x0b\x0c  "))
with open(sys.argv[3], "w") as f, open(sys.argv[4], "w") as o:
    for t in toks:
        f.write(t + "\n")
        d = R.resolve(t)
        o.write("-\n" if d is None else f"{d.lemma}|{d.gender}|{d.category}|{d.suffix}\n")
print(len(toks), "tokens")
