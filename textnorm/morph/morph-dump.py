#!/usr/bin/python3 -I
"""morph-dump.py < words > out.jsonl: the pymorphy3 reference for morph-dump.c.

Uses the pure-Python DAWG reader, as the oracle (trace.py) does."""
import json
import sys

sys.modules["dawg"] = None
import pymorphy3  # noqa: E402

NINFL_PARSES = 3
REQS = [
    "nomn,sing", "gent,sing", "datv,sing", "accs,sing", "ablt,sing", "loct,sing",
    "nomn,plur", "gent,plur", "datv,plur", "accs,plur", "ablt,plur", "loct,plur",
    "nomn", "gent", "datv", "accs", "ablt", "loct", "gen2", "loc2", "voct",
    "masc", "femn", "neut", "plur", "masc,ablt", "femn,gent",
]
REQSETS = [set(r.split(",")) for r in REQS]

morph = pymorphy3.MorphAnalyzer()


def jp(p):
    return [p.word, str(p.tag), p.normal_form, p.score]


for line in sys.stdin:
    w = line[:-1] if line.endswith("\n") else line
    ps = morph.parse(w)
    lex, infl = [], []
    for p in ps[:NINFL_PARSES]:
        try:
            lex.append([jp(x) for x in p.lexeme])
        except Exception:
            lex.append([])
        row = []
        for r in REQSETS:
            try:
                x = p.inflect(r)
            except Exception:
                x = None
            row.append(jp(x) if x is not None else None)
        infl.append(row)
    print(json.dumps({"w": w, "k": int(bool(morph.word_is_known(w))), "p": [jp(p) for p in ps],
                      "l": lex, "i": infl}, ensure_ascii=False, separators=(",", ":")))
